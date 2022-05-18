#include "slowMotionService.hpp"
#include "videoProcessor.hpp"
#include "model.cpp"
#include "config.hpp"

#include <zhelpers.hpp>
#include <torch/script.h>
#include <iostream>

using namespace std;

// singleton class instance
SlowMotionService *SlowMotionService::slowMotionService_ = nullptr;

/**
 * @brief Create a singleton instance of FlowVectorService::FlowVectorService class
 */
SlowMotionService* SlowMotionService::GetInstance(string inputPath, int slowmoFactor, int outputFps, string outputPath) {
    if (slowMotionService_ == nullptr) {
        slowMotionService_ = new SlowMotionService(inputPath, slowmoFactor, outputFps, outputPath);
    }

    return slowMotionService_;
}

/**
 * @brief Create a SlowMotionService::SlowMotionService object to handle all operations required for
 * interpolated frame generation
 *
 * @param inputPath specifying path to input video file
 * @param slowmoFactor specifying how much the input video needs to be slowed down by
 * @param outputFps specifying fps of output video
 * @param outputPath specifying path to output video file
 */
SlowMotionService::SlowMotionService(string inputPath, int slowmoFactor, int outputFps, string outputPath) {
    // create context
    context = zmq::context_t(1);

    // initialize requester socket on localhost:8080
    flowRequester = zmq::socket_t(context, ZMQ_REQ);
    cout << "SMS: Connecting responder to " << FV_SOCKET_PATH << "..." << endl;
	flowRequester.connect(FV_SOCKET_PATH);

    // initialize variables
    this->inputPath = inputPath;
    this->slowmoFactor = slowmoFactor;
    this->outputFps = outputFps;
    this->outputPath = outputPath;

    // create frame reader to interface directly with video frames
    videoProcessor = VideoProcessor::GetInstance(inputPath, slowmoFactor);

    // initialize models from torchscript
    // try {
    //     interpolationModel = torch::jit::load(FRAME_INTERPOLATION_MODEL_PATH);
    // } catch (const c10::Error& e) {
    //     cout << "Error loading Frame Interpolation model\n" << endl;
    //     exit(EXIT_FAILURE);
    // }
    cout << "SMS: Interpolation Model is loaded." << endl;

    try {
        stringstream command;
        command << "python3 " << MODEL_SCRIPT << " --model BackWarp --width " << videoProcessor->getVideoWidth() << " --height " << videoProcessor->getVideoHeight();
        system(command.str().c_str());
        backWarpModel = torch::jit::load(BACKWARP_MODEL_PATH);
    } catch (const c10::Error& e) {
        cerr << "Error loading BackWarp model\n" << endl;
        exit(EXIT_FAILURE);
    }
    cout << "SMS: BackWarp Model is loaded." << endl;

    // load models to device
    torch::DeviceType device = torch::kCUDA;
    // interpolationModel.to(device);
    backWarpModel.to(device);
}

/**
 * @brief Process video frames in pairs and created interpolated frames
 */
void SlowMotionService::startService() {
    torch::DeviceType device = torch::kCUDA;
    int currFrameIndex = 0;

    while (currFrameIndex < videoProcessor->getVideoFrameCount() * slowmoFactor - slowmoFactor) {
        // send request with frame number
        s_send(flowRequester, to_string(currFrameIndex));
        
        // receive message
        string serializedMsg = s_recv(flowRequester);

        // deserialize message into FlowVectorFrame
        stringstream msg(serializedMsg);
        boost::archive::binary_iarchive deserializer(msg);
        deserializer >> bufferFrame;

        // wrong object received
        if (currFrameIndex != bufferFrame.getFrameIndex()) {
            cout << "SMS: Received flow vectors out of order. Retrying." << endl;
            continue;
        }

        // create F_0_1 and F_1_0
        torch::Tensor F_0_1 = torch::cat(
            {
                torch::from_blob(bufferFrame.getXFlowFor(), {1, 1, bufferFrame.getHeight(), bufferFrame.getWidth()}),
                torch::from_blob(bufferFrame.getYFlowFor(), {1, 1, bufferFrame.getHeight(), bufferFrame.getWidth()})
            },
        1);

        torch::Tensor F_1_0 = torch::cat(
            {
                torch::from_blob(bufferFrame.getXFlowBack(), {1, 1, bufferFrame.getHeight(), bufferFrame.getWidth()}),
                torch::from_blob(bufferFrame.getYFlowBack(), {1, 1, bufferFrame.getHeight(), bufferFrame.getWidth()})
            },
        1);
        
        F_0_1.to(device);
        F_1_0.to(device);
        
        // Get frames I0 and I1
        vector<torch::Tensor> framePair = videoProcessor->getFramePair(currFrameIndex);
        torch::Tensor I0 = framePair[0].to(device);
        torch::Tensor I1 = framePair[1].to(device);

        // generate intermediate frames
        for(float i = 1; i != slowmoFactor; i++) {
            float t = i / slowmoFactor;
            float fCoeff0 = -t * (1-t);
            float fCoeff1 = t * t;
            float fCoeff2 = (1 - t) * (1 - t);
            float fCoeff3 = fCoeff0;

            torch::Tensor F_t_0 = torch::add((fCoeff0 * F_0_1), (fCoeff1 * F_1_0));
            torch::Tensor F_t_1 = torch::add((fCoeff2 * F_0_1), (fCoeff3 * F_1_0));
            F_t_0.to(device);
            F_t_1.to(device);

            // first pass of backwarp
            std::vector<torch::jit::IValue> backWarpInput;
            backWarpInput.push_back(I0);
            backWarpInput.push_back(F_t_0);
            
            torch::Tensor g_I0_F_t_0 = this->backWarpModel.forward(backWarpInput).toTensor();
            g_I0_F_t_0.to(device);

            backWarpInput.clear();
          
            backWarpInput.push_back(I1);
            backWarpInput.push_back(F_t_1);
            
            torch::Tensor g_I1_F_t_1 = this->backWarpModel.forward(backWarpInput).toTensor();
            g_I1_F_t_1.to(device);

            backWarpInput.clear();

            // interpolation
            // TODO: do these need to be concatenated then placed as a single tensor into the vector??
            // std::vector<torch::jit::IValue> interpolationInput;
            // interpolationInput.push_back(I0);
            // interpolationInput.push_back(I1);
            // interpolationInput.push_back(F_0_1);
            // interpolationInput.push_back(F_1_0);
            // interpolationInput.push_back(F_t_1);
            // interpolationInput.push_back(F_t_0);
            // interpolationInput.push_back(g_I1_F_t_1);
            // interpolationInput.push_back(g_I0_F_t_0);

            // torch::Tensor interpOut = this->interpolationModel.forward(interpolationInput).toTensor();

            // torch::Tensor F_t_0_f = interpOut.index({torch::indexing::Slice(), torch::indexing::Slice(0,2)}) + F_t_0;
            // torch::Tensor F_t_1_f = interpOut.index({torch::indexing::Slice(), torch::indexing::Slice(2,4)}) + F_t_1;
            
            // torch::Tensor temp = interpOut.index({torch::indexing::Slice(), torch::indexing::Slice(4,5)});
            // torch::Tensor V_t_0 = torch::sigmoid(temp);
            // torch::Tensor V_t_1 = 1 - V_t_0;

            // second pass of backwarp network
            // backWarpInput.push_back(I0);
            // backWarpInput.push_back(F_t_0_f);

            // torch::Tensor g_I0_F_t_0_f = this->backWarpModel.forward(backWarpInput).toTensor();
            // backWarpInput.clear();

            // backWarpInput.push_back(I1);
            // backWarpInput.push_back(F_t_1_f);

            // torch::Tensor g_I1_F_t_1_f = this->backWarpModel.forward(backWarpInput).toTensor();
            // backWarpInput.clear();

            float wCoeff0 = 1 - t;
            float wCoeff1 = t;

            // creates img tensor, converts to vector of char, then written to interpolation directory
            // torch::Tensor Ft_p = (wCoeff0 * V_t_0 * g_I0_F_t_0_f + wCoeff1 * V_t_1 * g_I1_F_t_1_f ) / (wCoeff0 * V_t_0 + wCoeff1 * V_t_1);
            torch::Tensor Ft_p = wCoeff0 * g_I0_F_t_0 + wCoeff1 * g_I1_F_t_1;

            vector<char> imgFile = videoProcessor->tensorToYUV(Ft_p);

            stringstream pathBuilder;
            pathBuilder << YUV_PATH << "/" << setfill('0') << setw(MAX_FILE_DIGITS) << currFrameIndex + int(i) << ".yuv";
            string imgName = pathBuilder.str();

            ofstream writeOut(imgName,ofstream::binary);
            writeOut.write(&imgFile[0],imgFile.size());
        }

        currFrameIndex += slowmoFactor;
    }

    // send termination request
    s_send(flowRequester, to_string(TERMINATION_MSG));

    // TODO: reconstruct video

}
