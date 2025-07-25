//
//  ConvBufExecution.cpp
//  MNN
//
//  Created by MNN on 2019/02/28.
//  Copyright © 2018, Alibaba Group Holding Limited
//

#ifndef MNN_OPENCL_BUFFER_CLOSED

#include "ConvBufExecution.hpp"
#include "ConvBufWinograd.hpp"
#include "ConvSubgroupBufExecution.hpp"
#include "core/ConvolutionCommon.hpp"
#include "core/Backend.hpp"
#include "RasterBufExecution.hpp"
#include "ConvBufLowMemoryExecution.hpp"

namespace MNN {
namespace OpenCL {

ConvBufCommonExecution::ConvBufCommonExecution(Backend *backend) {
    mOpenCLBackend = static_cast<OpenCLBackend *>(backend);
}
ConvBufCommonExecution::ConvBufCommonExecution(const Convolution2D *conv2dParams, Backend *backend) {
    auto openclBackend       = (OpenCLBackend *)backend;
    int biasSize             = conv2dParams->common()->outputCount();
    int buffer_size = ROUND_UP(biasSize, 32);//pack to packN
    if(openclBackend->getPrecision() != BackendConfig::Precision_High) {
        buffer_size *= sizeof(half_float::half);
    } else {
        buffer_size *= sizeof(float);
    }

    mResource.reset(new ConvBufResource);
    mResource->mBias.reset(Tensor::createDevice<float>({1, 1, 1, ROUND_UP(biasSize, 32)}));
    backend->onAcquireBuffer(mResource->mBias.get(), Backend::STATIC);
    cl::Buffer &biasBuffer = openCLBuffer(mResource->mBias.get());

    cl_int res;
    auto biasPtrCL = openclBackend->getOpenCLRuntime()->commandQueue().enqueueMapBuffer(
        biasBuffer, true, CL_MAP_WRITE, 0, buffer_size, nullptr, nullptr, &res);
    if(biasPtrCL != nullptr && res == CL_SUCCESS){
        ::memset(biasPtrCL, 0, buffer_size);
        if (nullptr != conv2dParams->bias()) {
            const float *biasDataPtr = conv2dParams->bias()->data();
            if(openclBackend->getPrecision() != BackendConfig::Precision_High){
                for(int i=0; i<biasSize; i++) {
                    ((half_float::half*)biasPtrCL)[i] = (half_float::half)(biasDataPtr[i]);
                }
            }else{
                ::memcpy(biasPtrCL, biasDataPtr, biasSize * sizeof(float));
            }
        }
    }else{
        MNN_ERROR("Map error biasPtrCL == nullptr \n");
    }
    openclBackend->getOpenCLRuntime()->commandQueue().enqueueUnmapMemObject(biasBuffer, biasPtrCL);
}

ConvBufCommonExecution::ConvBufCommonExecution(const Op *op, Backend *backend, bool isExtra) {
    mResource.reset(new ConvBufResource);
    auto openclBackend = (OpenCLBackend *)backend;
    cl_int res;
    const Convolution2D *conv2dParams = nullptr;
    if(isExtra){
        conv2dParams = flatbuffers::GetRoot<Convolution2D>(op->main_as_Extra()->attr()->GetAs<Attribute>(0)->tensor()->uint8s()->data());
    }else{
        conv2dParams = op->main_as_Convolution2D();
    }
    int biasSize             = conv2dParams->common()->outputCount();
    int buffer_size = ROUND_UP(biasSize, 32);//pack to packN
    if(openclBackend->getPrecision() != BackendConfig::Precision_High) {
        buffer_size *= sizeof(half_float::half);
    } else {
        buffer_size *= sizeof(float);
    }
    
    mResource.reset(new ConvBufResource);
    mResource->mBias.reset(Tensor::createDevice<float>({1, 1, 1, ROUND_UP(biasSize, 32)}));
    backend->onAcquireBuffer(mResource->mBias.get(), Backend::STATIC);
    cl::Buffer &biasBuffer = openCLBuffer(mResource->mBias.get());
    
    auto biasPtrCL = openclBackend->getOpenCLRuntime()->commandQueue().enqueueMapBuffer(biasBuffer, true, CL_MAP_WRITE, 0, buffer_size, nullptr, nullptr, &res);
    if(biasPtrCL != nullptr && res == CL_SUCCESS){
        ::memset(biasPtrCL, 0, buffer_size);
        if (nullptr != conv2dParams->bias()) {
            const float *biasDataPtr = conv2dParams->bias()->data();
            if(openclBackend->getPrecision() != BackendConfig::Precision_High){
                for(int i=0; i<biasSize; i++) {
                    ((half_float::half*)biasPtrCL)[i] = (half_float::half)(biasDataPtr[i]);
                }
            }else{
                ::memcpy(biasPtrCL, biasDataPtr, biasSize * sizeof(float));
            }
        }
    }else{
        MNN_ERROR("Map error biasPtrCL == nullptr \n");
    }
    
    openclBackend->getOpenCLRuntime()->commandQueue().enqueueUnmapMemObject(biasBuffer, biasPtrCL);
    if(isExtra){
        const PRelu* preluParam = flatbuffers::GetRoot<PRelu>(op->main_as_Extra()->attr()->GetAs<Attribute>(1)->tensor()->uint8s()->data());
        const float *slopeDataPtr = preluParam->slope()->data();
        mResource->mSlope.reset(Tensor::createDevice<float>({1, 1, 1, ROUND_UP(biasSize, 32)}));
        backend->onAcquireBuffer(mResource->mSlope.get(), Backend::STATIC);
        cl::Buffer &slopeBuffer = openCLBuffer(mResource->mSlope.get());
            
        auto slopePtrCL = openclBackend->getOpenCLRuntime()->commandQueue().enqueueMapBuffer(slopeBuffer, true, CL_MAP_WRITE, 0, buffer_size, nullptr, nullptr, &res);
        if(slopePtrCL != nullptr && res == CL_SUCCESS){
            if(openclBackend->getPrecision() != BackendConfig::Precision_High){
                for(int i=0; i<biasSize; i++) {
                    ((half_float::half*)slopePtrCL)[i] = (half_float::half)(slopeDataPtr[i]);
                }
            }else{
                ::memcpy(slopePtrCL, slopeDataPtr, biasSize * sizeof(float));
            }
        }else{
            MNN_ERROR("Map error slopePtrCL == nullptr \n");
        }
        openclBackend->getOpenCLRuntime()->commandQueue().enqueueUnmapMemObject(slopeBuffer, slopePtrCL);
    }
}

ConvBufCommonExecution::~ConvBufCommonExecution() {
    // Do nothing
}

void ConvBufExecution::_generateFilterConvertRegion(Tensor* virtualFilter, Tensor* originBuffer) const {
    auto filterDes = TensorUtils::getDescribe(virtualFilter);
    filterDes->regions.clear();
    for (int so=0; so<4; ++so) {
        int oSize = (mResource->mOutputChannel - so + 3) / 4;
        if (oSize <= 0) {
            continue;
        }
        Tensor::InsideDescribe::Region slice;
        slice.origin = originBuffer;
        slice.size[0] = oSize;
        slice.size[1] = mResource->mInputChannel;
        slice.size[2] = mResource->mKernelWidth * mResource->mKernelHeight;
        slice.src.stride[0] = mResource->mInputChannel * mResource->mKernelWidth * mResource->mKernelHeight * 4;
        slice.src.stride[1] = mResource->mKernelWidth * mResource->mKernelHeight;
        slice.src.stride[2] = 1;
        slice.src.offset = so * mResource->mInputChannel * mResource->mKernelWidth * mResource->mKernelHeight;
        slice.dst.stride[0] = mResource->mKernelWidth * mResource->mKernelHeight * 4;
        slice.dst.stride[1] = mResource->mKernelWidth * mResource->mKernelHeight * UP_DIV(mResource->mOutputChannel, 4) * 4;
        slice.dst.stride[2] = 4;
        slice.dst.offset = so;
        filterDes->regions.emplace_back(std::move(slice));
    }
}

ConvBufExecution::ConvBufExecution(const std::vector<Tensor *> &inputs, const std::vector<Tensor *> &outputs, const MNN::Op *op, Backend *backend, bool isExtra)
    : ConvBufCommonExecution(op, backend, isExtra), CommonExecution(backend, op) {
#ifdef LOG_VERBOSE
    MNN_PRINT("Start ConvExecution init !\n");
#endif
    mOpenCLBackend                 = static_cast<OpenCLBackend *>(backend);
    const Convolution2D* conv2dParams = nullptr;
    if(isExtra){
        conv2dParams = flatbuffers::GetRoot<Convolution2D>(op->main_as_Extra()->attr()->GetAs<Attribute>(0)->tensor()->uint8s()->data());
        mResource->mPrelu = true;
    }else{
        conv2dParams       = op->main_as_Convolution2D();
    }
    const auto *conv2dCommonParams = conv2dParams->common();
    mResource->mConv2dParams                  = conv2dParams;
    mResource->mConv2dCommonParams            = conv2dCommonParams;
    mResource->mStrides                       = {conv2dCommonParams->strideY(), conv2dCommonParams->strideX()};
    mResource->mDilations                     = {conv2dCommonParams->dilateY(), conv2dCommonParams->dilateX()};
    auto padding = ConvolutionCommon::convolutionPad(inputs[0], outputs[0], mResource->mConv2dCommonParams);
    mPaddings[0] = padding.second;//padY
    mPaddings[1] = padding.first;//padX
    mResource->mKernelWidth   = conv2dCommonParams->kernelX();
    mResource->mKernelHeight  = conv2dCommonParams->kernelY();
    mResource->mOutputChannel = conv2dCommonParams->outputCount();
    mResource->mInputChannel = inputs[0]->channel();
    mResource->mRelu = conv2dCommonParams->relu();
    mResource->mRelu6 = conv2dCommonParams->relu6();
        
    std::shared_ptr<ConvolutionCommon::Int8Common> quanCommon;
    if (inputs.size() != 1) {
        // Multi - Input
        mResource->mConv1x1Opt = false;
        mResource->mRasterExe.reset(new RasterBufExecution({mResource->mFilter.get()}, op, mOpenCLBackend));
    } else {
        int weightSize   = 0;
        if (nullptr != conv2dParams->quanParameter()) {
            bool forceFloat = conv2dParams->quanParameter()->index() != nullptr;
            quanCommon = ConvolutionCommon::load(op, backend, forceFloat);
            mFilterDataPtr = quanCommon->weightFloat.get();
            weightSize = quanCommon->weightFloat.size();
        }
        if (mFilterDataPtr == nullptr) {
            mFilterDataPtr = conv2dParams->weight()->data();
            weightSize = conv2dParams->weight()->size();
        }
        //select opt conv method
        bool isConv1x1 = (mResource->mKernelHeight == mResource->mKernelWidth && mResource->mKernelHeight == 1 && mPaddings[0] == 0 &&
                          mPaddings[1] == 0 && mResource->mStrides[0] == 1 && mResource->mStrides[1] == 1);

        mResource->mConv1x1Opt = isConv1x1;
        if(mResource->mConv1x1Opt) {
            mResource->mAlignK = 4;
            mResource->mAlignN = 8;
        }
        bool useConvGemm = isConv1x1 && mResource->mInputChannel > 32 && mResource->mOutputChannel > 64;
        if (useConvGemm) {
            mResource->mAlignK = 4;
            mResource->mAlignN = 16;
            mResource->mConvGemmOptLevel = 1;
            if(mResource->mOutputChannel > 1024) {
                mResource->mAlignN = 128;
            } else if(mResource->mOutputChannel > 512) {
                mResource->mAlignN = 64;
            } else if(mResource->mOutputChannel > 96) {
                mResource->mAlignN = 32;
            }
        }
    }
    if (mResource->mConv1x1Opt) {
        int buffer_size = ROUND_UP(mResource->mOutputChannel, mResource->mAlignN) * ROUND_UP(mResource->mInputChannel, mResource->mAlignK);
        mResource->mFilter.reset(
            Tensor::createDevice<float>({buffer_size}));
        mOpenCLBackend->onAcquireBuffer(mResource->mFilter.get(), Backend::STATIC);

        if (mOpenCLBackend->getPrecision() != BackendConfig::Precision_High) {
            buffer_size *= sizeof(half_float::half);
        } else {
            buffer_size *= sizeof(float);
        }

        cl::Buffer &filterBuffer = openCLBuffer(mResource->mFilter.get());
        cl_int error;
        auto ptrCL = mOpenCLBackend->getOpenCLRuntime()->commandQueue().enqueueMapBuffer(
                filterBuffer, true, CL_MAP_WRITE, 0, buffer_size, nullptr, nullptr, &error);
        if(nullptr != ptrCL && error == CL_SUCCESS){
            memset((void *)ptrCL, 0, buffer_size);
            if (mOpenCLBackend->getPrecision() != BackendConfig::Precision_High) {
                // [Ci, Co] ( [K, N] )
                for (int o = 0; o < mResource->mOutputChannel; o++) {
                    for (int i = 0; i < mResource->mInputChannel; i++) {
                        ((half_float::half *)ptrCL)[i * ROUND_UP(mResource->mOutputChannel, mResource->mAlignN) + o] = (half_float::half)(mFilterDataPtr[o * mResource->mInputChannel + i]);
                    }
                }
            } else {
                for (int o = 0; o < mResource->mOutputChannel; o++) {
                    for (int i = 0; i < mResource->mInputChannel; i++) {
                        ((float *)ptrCL)[i * ROUND_UP(mResource->mOutputChannel, mResource->mAlignN) + o] = (mFilterDataPtr[o * mResource->mInputChannel + i]);
                    }
                }
            }
        }else{
            MNN_ERROR("Map error filterPtrCL == nullptr \n");
        }
        mOpenCLBackend->getOpenCLRuntime()->commandQueue().enqueueUnmapMemObject(filterBuffer, ptrCL);

    } else {
        mResource->mFilter.reset(
            Tensor::createDevice<float>({ROUND_UP(mResource->mOutputChannel, 4) * ROUND_UP(mResource->mInputChannel, 4) * mResource->mKernelWidth * mResource->mKernelHeight}));
        if (mFilterDataPtr != nullptr) {
            std::vector<int> filterImageShape{ROUND_UP(mResource->mInputChannel, 4), (UP_DIV(mResource->mOutputChannel, 4) * mResource->mKernelWidth * mResource->mKernelHeight)};
            std::shared_ptr<Tensor> filterBuffer(
                Tensor::createDevice<float>({mResource->mOutputChannel, ROUND_UP(mResource->mInputChannel, 4), mResource->mKernelWidth, mResource->mKernelHeight}));

            int buffer_size = filterBuffer->elementSize() * sizeof(float);
            cl::Buffer filterBufferCL(mOpenCLBackend->getOpenCLRuntime()->context(), CL_MEM_READ_WRITE | CL_MEM_ALLOC_HOST_PTR, buffer_size);
            filterBuffer->buffer().device = (uint64_t)(&filterBufferCL);

            cl_int res;
            auto ptrCL = mOpenCLBackend->getOpenCLRuntime()->commandQueue().enqueueMapBuffer(filterBufferCL, true, CL_MAP_WRITE, 0, buffer_size, nullptr, nullptr, &res);
            if(ptrCL != nullptr && res == CL_SUCCESS) {
                ::memset(ptrCL, 0, buffer_size);
                const int copy_size = mResource->mKernelWidth * mResource->mKernelHeight * sizeof(float);
                for(int oc=0; oc<mResource->mOutputChannel; oc++) {
                    for(int ic=0; ic<mResource->mInputChannel; ic++) {
                        ::memcpy((float *)ptrCL + (oc * ROUND_UP(mResource->mInputChannel, 4) + ic) * mResource->mKernelWidth * mResource->mKernelHeight, mFilterDataPtr + (oc * mResource->mInputChannel + ic) * mResource->mKernelWidth * mResource->mKernelHeight, copy_size);
                    }
                }
            }else{
                MNN_ERROR("Map error ptrCL == nullptr \n");
            }
            mOpenCLBackend->getOpenCLRuntime()->commandQueue().enqueueUnmapMemObject(filterBufferCL, ptrCL);

            mResource->mFilter.reset(Tensor::createDevice<float>({filterImageShape[1] * 4 * filterImageShape[0]}));
            mOpenCLBackend->onAcquireBuffer(mResource->mFilter.get(), Backend::STATIC);
            MNN::OpenCL::BufferConvertor bufferConvertor{mOpenCLBackend->getOpenCLRuntime()};

            bool needTrans = true;
            bufferConvertor.convertToNC4HW4Buffer(filterBuffer.get(), MNN::OpenCL::CONV2D_FILTER, mResource->mFilter.get(), mOpenCLBackend->getPrecision(), needTrans);
        }
    }

    if (mResource->mRelu) {
        mResource->mBuildOptions.emplace("-DRELU");
    } else if (mResource->mRelu6) {
        mResource->mBuildOptions.emplace("-DRELU6");
    } else if(mResource->mPrelu){
        mResource->mBuildOptions.emplace("-DPRELU");
    }

#ifdef LOG_VERBOSE
    MNN_PRINT("end ConvExecution init !\n");
#endif
}

ConvBufExecution::~ConvBufExecution() {
    // Do nothing
}

ConvBufExecution::ConvBufExecution(std::shared_ptr<ConvBufResource> resource, const MNN::Op* op, Backend *backend)
    : ConvBufCommonExecution(backend), CommonExecution(backend, op) {
    mResource = resource;
    const auto *conv2dParams       = op->main_as_Convolution2D();
    const auto *conv2dCommonParams = conv2dParams->common();
    mResource->mConv2dParams       = conv2dParams;
    mResource->mConv2dCommonParams  = conv2dCommonParams;
}

bool ConvBufExecution::onClone(Backend* bn, const Op* op, Execution** dst) {
    if (!mValid) {
        return false;
    }
    if (nullptr == dst) {
        return true;
    }
    *dst = new ConvBufExecution(mResource, op, bn);
    return true;
}

ErrorCode ConvBufExecution::onResize(const std::vector<Tensor *> &inputs, const std::vector<Tensor *> &outputs) {
#ifdef LOG_VERBOSE
    MNN_PRINT("Start ConvExecution onResize !\n");
#endif
    mKernel.resize(1);
    auto input  = inputs[0];
    auto output = outputs[0];
    if (inputs.size() > 1) {
        // Multi Input, need pretreat
        _generateFilterConvertRegion(mResource->mFilter.get(), inputs[1]);
        bool res = backend()->onAcquireBuffer(mResource->mFilter.get(), Backend::DYNAMIC);
        if (!res) {
            return OUT_OF_MEMORY;
        }
        mResource->mRasterExe->onResize({}, {mResource->mFilter.get()});
    }
    mOpenCLBackend->startRecord(mRecording);
    std::vector<int> inputShape  = tensorShapeFormat(input);
    std::vector<int> outputShape = tensorShapeFormat(output);
    const int batch              = outputShape.at(0);
    const int height             = outputShape.at(1);
    const int width              = outputShape.at(2);
    const int outChannel         = outputShape.at(3);

    const int inputHeight   = inputShape.at(1);
    const int inputWidth    = inputShape.at(2);
    const int inputChannels = inputShape.at(3);

    const int inputChannelBlocks = UP_DIV(inputChannels, 4);
    
    auto pad = ConvolutionCommon::convolutionPad(input, output, mResource->mConv2dCommonParams);
    mPaddings[0] = pad.second;//padY
    mPaddings[1] = pad.first;//padX

    // printf("nchw %d %d %d %d, cohw %d %d %d, khw %d %d  gemm:%d \n", inputs[0]->batch(), inputs[0]->channel(), inputs[0]->height(), inputs[0]->width(), outputs[0]->channel(), outputs[0]->height(), outputs[0]->width(), mResource->mKernelWidth, mResource->mKernelHeight, mResource->mConvGemmOptLevel);

    std::string info = std::to_string(inputChannels) + "_" + std::to_string(outChannel) + "_" + std::to_string(mResource->mKernelHeight) + "_" + std::to_string(mResource->mKernelWidth) + "_" + std::to_string(mResource->mStrides[0]) + "_" + std::to_string(mResource->mStrides[1]) + "_" + std::to_string(mResource->mDilations[0]) + "_" + std::to_string(mResource->mDilations[1]);

    if (mResource->mConvGemmOptLevel > 0) {
        int area = height * width;
        int M = outputShape.at(0) * area;
        int N = outputShape.at(3);
        int K = inputShape.at(3);
        
        // total computation not enough
       if(M < 128 || 1.0 * M / 512 * N / 512 * K / 256 < 1.0) {
            mResource->mConvGemmOptLevel = 0;
        }
    }
    
    if (mResource->mConvGemmOptLevel == 1) {
        int area = height * width;
        int M = outputShape.at(0) * area;
        int N = outputShape.at(3);
        int K = inputShape.at(3);
        // set M Align
        float ratio = 1.0 * M / 1024.0 * N / 1024.0 * K / 1024.0;
        if(M > 1024 && ratio >= 1.0) {
            mAlignM = 128;
        } else if(M > 512 && ratio >= 0.1) {
            mAlignM = 64;
        } else if(M > 96){
            mAlignM = 32;
        } else {
            mAlignM = 16;
        }

        int alignM = ROUND_UP(M, mAlignM);
        int alignN = ROUND_UP(N, mResource->mAlignN);
        int alignK = ROUND_UP(K, mResource->mAlignK);

        // ReArrange input
        mConvGemmInpTensor.reset(Tensor::createDevice<float>({alignK * alignM}));
        mOpenCLBackend->onAcquireBuffer(mConvGemmInpTensor.get(), Backend::DYNAMIC);
        mConvGemmOutTensor.reset(Tensor::createDevice<float>({alignN * alignM}));
        mOpenCLBackend->onAcquireBuffer(mConvGemmOutTensor.get(), Backend::DYNAMIC);
        
        {
            std::set<std::string> buildOptions;
            
            int m_pack = 4;
            mPreKernel = mOpenCLBackend->getOpenCLRuntime()->buildKernel("gemm_buf", "transpose_pad", buildOptions, mOpenCLBackend->getPrecision());
            uint32_t maxWorkGroupSize = static_cast<uint32_t>(mOpenCLBackend->getOpenCLRuntime()->getMaxWorkGroupSize(mPreKernel));
            mPreGlobalWorkSize = {static_cast<uint32_t>(alignM/m_pack), static_cast<uint32_t>(alignK/4)};

            int offset = 0;
            int idx            = 0;
            cl_int ret = CL_SUCCESS;
            ret |= mPreKernel->get().setArg(idx++, static_cast<int>(mPreGlobalWorkSize[0]));
            ret |= mPreKernel->get().setArg(idx++, static_cast<int>(mPreGlobalWorkSize[1]));
            ret |= mPreKernel->get().setArg(idx++, static_cast<int>(alignM));
            ret |= mPreKernel->get().setArg(idx++, static_cast<int>(alignK));
            ret |= mPreKernel->get().setArg(idx++, static_cast<int>(M));
            ret |= mPreKernel->get().setArg(idx++, static_cast<int>(K));
            ret |= mPreKernel->get().setArg(idx++, static_cast<int>(area));
            ret |= mPreKernel->get().setArg(idx++, openCLBuffer(input));
            ret |= mPreKernel->get().setArg(idx++, openCLBuffer(mConvGemmInpTensor.get()));
            MNN_CHECK_CL_SUCCESS(ret, "setArg mConvgemmOptLevel==1 PreKernel");
            mPreLocalWorkSize = localWS2DDefault(mPreGlobalWorkSize, maxWorkGroupSize, mOpenCLBackend->getOpenCLRuntime(), "transpose_pad", mPreKernel, mOpenCLBackend->getCLTuneLevel(), "gemm_buf").first;

            mOpenCLBackend->recordKernel2d(mPreKernel, mPreGlobalWorkSize, mPreLocalWorkSize);
            mPreGlobalWorkSize[0] = ROUND_UP(mPreGlobalWorkSize[0], std::max((uint32_t)1, mPreLocalWorkSize[0]));
            mPreGlobalWorkSize[1] = ROUND_UP(mPreGlobalWorkSize[1], std::max((uint32_t)1, mPreLocalWorkSize[1]));
        }
        
        // call gemm strassen
        {
            mStrassenComputor.reset(new StrassenMatrixComputor(backend(), 3));
            mStrassenComputor->onEncode(alignM, alignK, alignN, alignM, alignN, alignN, openCLBuffer(mConvGemmInpTensor.get()), openCLBuffer(mResource->mFilter.get()), openCLBuffer(mConvGemmOutTensor.get()),
                                         false, openCLBuffer(mResource->mBias.get()));
        }
        
        // call output transpose
        {
            std::set<std::string> buildOptions = mResource->mBuildOptions;
            int pack_m = 1;
            if(M % 8 == 0) {
                pack_m = 8;
            } else if(M % 4 == 0) {
                pack_m = 4;
            }
            buildOptions.emplace("-DM_VEC=" + std::to_string(pack_m));
            mPostKernel = mOpenCLBackend->getOpenCLRuntime()->buildKernel("gemm_buf", "transpose_bias", buildOptions, mOpenCLBackend->getPrecision());
            uint32_t maxWorkGroupSize = static_cast<uint32_t>(mOpenCLBackend->getOpenCLRuntime()->getMaxWorkGroupSize(mPostKernel));

            mPostGlobalWorkSize = {static_cast<uint32_t>(UP_DIV(M, pack_m)), static_cast<uint32_t>(UP_DIV(N, 4))};

            int offset = 0;
            int idx            = 0;
            cl_int ret = CL_SUCCESS;
            ret |= mPostKernel->get().setArg(idx++, static_cast<int>(mPostGlobalWorkSize[0]));
            ret |= mPostKernel->get().setArg(idx++, static_cast<int>(mPostGlobalWorkSize[1]));
            ret |= mPostKernel->get().setArg(idx++, static_cast<int>(alignM));
            ret |= mPostKernel->get().setArg(idx++, static_cast<int>(alignN));
            ret |= mPostKernel->get().setArg(idx++, static_cast<int>(M));
            ret |= mPostKernel->get().setArg(idx++, static_cast<int>(N));
            ret |= mPostKernel->get().setArg(idx++, static_cast<int>(area));
            ret |= mPostKernel->get().setArg(idx++, openCLBuffer(mConvGemmOutTensor.get()));
            ret |= mPostKernel->get().setArg(idx++, openCLBuffer(mResource->mBias.get()));
            ret |= mPostKernel->get().setArg(idx++, openCLBuffer(output));
            if(mResource->mPrelu){
                ret |= mPostKernel->get().setArg(idx++, openCLBuffer(mResource->mSlope.get()));
            }

            MNN_CHECK_CL_SUCCESS(ret, "setArg mConvgemmOptLevel==1 PostKernel");
            mPostLocalWorkSize = localWS2DDefault(mPostGlobalWorkSize, maxWorkGroupSize, mOpenCLBackend->getOpenCLRuntime(), "transpose_bias", mPostKernel, mOpenCLBackend->getCLTuneLevel(), "gemm_buf").first;
            mOpenCLBackend->recordKernel2d(mPostKernel, mPostGlobalWorkSize, mPostLocalWorkSize);
            mPostGlobalWorkSize[0] = ROUND_UP(mPostGlobalWorkSize[0], std::max((uint32_t)1, mPostLocalWorkSize[0]));
            mPostGlobalWorkSize[1] = ROUND_UP(mPostGlobalWorkSize[1], std::max((uint32_t)1, mPostLocalWorkSize[1]));

            mOpenCLBackend->endRecord(mRecording);
        }
        mOpenCLBackend->onReleaseBuffer(mConvGemmInpTensor.get(), Backend::DYNAMIC);
        mOpenCLBackend->onReleaseBuffer(mConvGemmOutTensor.get(), Backend::DYNAMIC);
        
        return NO_ERROR;
    } else if (mResource->mConv1x1Opt) {
        if(inputChannels >= 128 && outputShape[0] * outChannel * width * height <= 64){
            mResource->mConv1x1Local = true;
            int local_size = 1;
            while(local_size * 2 <= 256 && local_size * 2 <= inputChannelBlocks){
                local_size *= 2;
            }
            mGlobalWorkSize = {static_cast<uint32_t>(local_size), static_cast<uint32_t>(UP_DIV(outChannel, 4) * width), static_cast<uint32_t>(outputShape[0] * height)};
            mLocalWorkSize = {static_cast<uint32_t>(local_size), 1, 1};
            
            std::set<std::string> buildOption = mResource->mBuildOptions;
            buildOption.emplace("-DCONV_LOCAL_SIZE=" + std::to_string(local_size));
            mKernel[0]        = mOpenCLBackend->getOpenCLRuntime()->buildKernel("conv_2d_buf", "conv_2d_1x1_local", buildOption, mOpenCLBackend->getPrecision());
            uint32_t idx = 0;
            cl_int ret = CL_SUCCESS;

            ret |= mKernel[0]->get().setArg(idx++, UP_DIV(width, 1));
            ret |= mKernel[0]->get().setArg(idx++, openCLBuffer(input));
            ret |= mKernel[0]->get().setArg(idx++, openCLBuffer(mResource->mFilter.get()));
            ret |= mKernel[0]->get().setArg(idx++, openCLBuffer(mResource->mBias.get()));
            ret |= mKernel[0]->get().setArg(idx++, openCLBuffer(output));
            ret |= mKernel[0]->get().setArg(idx++, static_cast<int>(inputChannelBlocks));
            ret |= mKernel[0]->get().setArg(idx++, batch);
            ret |= mKernel[0]->get().setArg(idx++, height);
            ret |= mKernel[0]->get().setArg(idx++, width);
            ret |= mKernel[0]->get().setArg(idx++, UP_DIV(outChannel, 4));
            ret |= mKernel[0]->get().setArg(idx++, ROUND_UP(outChannel, mResource->mAlignN));
            if(mResource->mPrelu){
                ret |= mKernel[0]->get().setArg(idx++, openCLBuffer(mResource->mSlope.get()));
            }
            MNN_CHECK_CL_SUCCESS(ret, "setArg Conv1x1Buf");
        } else {
            mResource->mConv1x1Local = false;
            // {"conv_2d_1x1_c4h1w4", "conv_2d_1x1_c4h1w2", "conv_2d_1x1_c4h1w1", "conv_2d_1x1_c8h1w4"};
            const int total_kernel = 3;
            std::string kernelName[total_kernel] = {"conv_2d_1x1_c4h1w4", "conv_2d_1x1_c4h1w2", "conv_2d_1x1_c4h1w1"};
            int itemC[total_kernel] = {4, 4, 4};
            int itemW[total_kernel] = {4, 2, 1};

            int M = outputShape.at(0) * outputShape.at(1) * outputShape.at(2);
            mResource->mConv1x1C8Opt = (mResource->mOutputChannel >= 16 && M >= 16 && M * mResource->mOutputChannel >= 65536);
            
            int actual_kernel = total_kernel;
            if(mResource->mConv1x1C8Opt) {
                actual_kernel = 2;
                kernelName[0] = "conv_2d_1x1_c8h1w4";
                itemC[0]      = 8;
                itemW[0]      = 4;

                kernelName[1] = "conv_2d_1x1_c8h1w2";
                itemC[1]      = 8;
                itemW[1]      = 2;
            }

            std::shared_ptr<KernelWrap> kernel[total_kernel];
            std::vector<uint32_t> globalWorkSize[total_kernel];
            std::vector<uint32_t> localWorkSize[total_kernel];
            std::pair<int, int> min_cost(INT_MAX, 0);//(min_time, min_index)
            for(int knl_idx = 0; knl_idx < actual_kernel; knl_idx++) {
                std::set<std::string> buildOption = mResource->mBuildOptions;
                if(itemC[knl_idx] == 8 && outputShape.at(3) % itemC[knl_idx] > 0 && outputShape.at(3) % itemC[knl_idx] <= 4){
                    buildOption.emplace("-DCHANNEL_BOUNDARY_PROTECT");
                }
                if((outputShape.at(2) % itemW[knl_idx]) != 0){
                    buildOption.emplace("-DBLOCK_LEAVE");
                }
                kernel[knl_idx]        = mOpenCLBackend->getOpenCLRuntime()->buildKernel("conv_2d_buf", kernelName[knl_idx], buildOption, mOpenCLBackend->getPrecision());
                uint32_t maxWorkGroupSize = static_cast<uint32_t>(mOpenCLBackend->getOpenCLRuntime()->getMaxWorkGroupSize(kernel[knl_idx]));
                
                uint32_t idx            = 0;
                cl_int ret = CL_SUCCESS;
                globalWorkSize[knl_idx] = {static_cast<uint32_t>(UP_DIV(outputShape.at(3), itemC[knl_idx]) * UP_DIV(outputShape.at(2), itemW[knl_idx])), static_cast<uint32_t>(outputShape.at(0) * outputShape.at(1))};

                ret |= kernel[knl_idx]->get().setArg(idx++, globalWorkSize[knl_idx][0]);
                ret |= kernel[knl_idx]->get().setArg(idx++, globalWorkSize[knl_idx][1]);
                ret |= kernel[knl_idx]->get().setArg(idx++, UP_DIV(width, itemW[knl_idx]));
                ret |= kernel[knl_idx]->get().setArg(idx++, openCLBuffer(input));
                ret |= kernel[knl_idx]->get().setArg(idx++, openCLBuffer(mResource->mFilter.get()));
                ret |= kernel[knl_idx]->get().setArg(idx++, openCLBuffer(mResource->mBias.get()));
                ret |= kernel[knl_idx]->get().setArg(idx++, openCLBuffer(output));
                ret |= kernel[knl_idx]->get().setArg(idx++, static_cast<int>(inputChannelBlocks));
                ret |= kernel[knl_idx]->get().setArg(idx++, height);
                ret |= kernel[knl_idx]->get().setArg(idx++, width);
                ret |= kernel[knl_idx]->get().setArg(idx++, batch);
                ret |= kernel[knl_idx]->get().setArg(idx++, UP_DIV(outChannel, 4));
                ret |= kernel[knl_idx]->get().setArg(idx++, ROUND_UP(outChannel, mResource->mAlignN));
                if(mResource->mPrelu){
                    ret |= kernel[knl_idx]->get().setArg(idx++, openCLBuffer(mResource->mSlope.get()));
                }

                MNN_CHECK_CL_SUCCESS(ret, "setArg Conv1x1Buf Kernel Select");

                std::pair<std::vector<uint32_t>, int> retTune;
                retTune = localWS2DDefault(globalWorkSize[knl_idx], maxWorkGroupSize, mOpenCLBackend->getOpenCLRuntime(), kernelName[knl_idx] + info, kernel[knl_idx], mOpenCLBackend->getCLTuneLevel(), "conv_2d_buf");
                if(min_cost.first > retTune.second) {
                    min_cost.first = retTune.second;
                    min_cost.second = knl_idx;
                    mLocalWorkSize = {retTune.first[0], retTune.first[1]};
                }
            }

            int min_index  = min_cost.second;
            mGlobalWorkSize = {globalWorkSize[min_index][0], globalWorkSize[min_index][1]};

            std::set<std::string> buildOption = mResource->mBuildOptions;
            if(itemC[min_index] == 8 && outputShape.at(3) % itemC[min_index] > 0 && outputShape.at(3) % itemC[min_index] <= 4){
                buildOption.emplace("-DCHANNEL_BOUNDARY_PROTECT");
            }
            if((outputShape.at(2) % itemW[min_index]) != 0){
                buildOption.emplace("-DBLOCK_LEAVE");
            }
            mKernel[0]        = mOpenCLBackend->getOpenCLRuntime()->buildKernel("conv_2d_buf", kernelName[min_index], buildOption, mOpenCLBackend->getPrecision());
            uint32_t idx = 0;
            cl_int ret = CL_SUCCESS;

            ret |= mKernel[0]->get().setArg(idx++, mGlobalWorkSize[0]);
            ret |= mKernel[0]->get().setArg(idx++, mGlobalWorkSize[1]);
            ret |= mKernel[0]->get().setArg(idx++, UP_DIV(width, itemW[min_index]));
            ret |= mKernel[0]->get().setArg(idx++, openCLBuffer(input));
            ret |= mKernel[0]->get().setArg(idx++, openCLBuffer(mResource->mFilter.get()));
            ret |= mKernel[0]->get().setArg(idx++, openCLBuffer(mResource->mBias.get()));
            ret |= mKernel[0]->get().setArg(idx++, openCLBuffer(output));
            ret |= mKernel[0]->get().setArg(idx++, static_cast<int>(inputChannelBlocks));
            ret |= mKernel[0]->get().setArg(idx++, height);
            ret |= mKernel[0]->get().setArg(idx++, width);
            ret |= mKernel[0]->get().setArg(idx++, batch);
            ret |= mKernel[0]->get().setArg(idx++, UP_DIV(outChannel, 4));
            ret |= mKernel[0]->get().setArg(idx++, ROUND_UP(outChannel, mResource->mAlignN));
            if(mResource->mPrelu){
                ret |= mKernel[0]->get().setArg(idx++, openCLBuffer(mResource->mSlope.get()));
            }
            MNN_CHECK_CL_SUCCESS(ret, "setArg Conv1x1Buf");
        }
    } else {
        int inputImageShape[2]  = {inputHeight, inputWidth};
        int outputImageShape[2] = {height, width};
        int kernelShape[2]      = {mResource->mKernelHeight, mResource->mKernelWidth};
        int strideShape[2]      = {mResource->mStrides[0],mResource->mStrides[1]};
        int paddingShape[2]     = {mPaddings[0], mPaddings[1]};
        int dilationShape[2]    = {mResource->mDilations[0], mResource->mDilations[1]};

        // {"conv_2d_c4h1w2", "conv_2d_c4h1w1", "conv_2d_c8h1w1", "conv_2d_c4h1w4", "conv_2d_c8h2w1", "conv_2d_c4h4w1"};
        const int total_kernel = 7;
        std::string kernelName[total_kernel] = {"conv_2d_c4h1w1", "conv_2d_c4h1w2", "conv_2d_c4h4w1",  "conv_2d_c4h1w4", "conv_2d_c8h2w1", "conv_2d_c8h4w1", "conv_2d_c8h1w4"};
        int itemC[total_kernel] = {4, 4, 4, 4, 8, 8, 8};
        int itemH[total_kernel] = {1, 1, 4, 1, 2, 4, 1};
        int itemW[total_kernel] = {1, 2, 1, 4, 1, 1, 4};

        int actual_kernel = total_kernel;
        int outChannelBlocks = UP_DIV(outChannel, 4);
        int conv_block_num = 1;
        auto magic_ratio = 1.0 * outputShape.at(0) * outputShape.at(1) * outputShape.at(2) / 1024.0 * \
                            inputChannels * kernelShape[0] * kernelShape[1] / 1024.0 * \
                            outChannel / 1024.0;
        if(magic_ratio >= 16.0 && outChannelBlocks >= 64) {
            conv_block_num = 8;
        } else if(magic_ratio >= 8.0 && outChannelBlocks >= 32) {
            conv_block_num = 4;
        } else if(magic_ratio >= 4.0 && outChannelBlocks >= 16) {
            conv_block_num = 2;
        } else {
            conv_block_num = 1;
        }

        mKernel.resize(conv_block_num);
        
        std::shared_ptr<KernelWrap> kernel[total_kernel];
        std::vector<uint32_t> globalWorkSize[total_kernel];
        std::vector<uint32_t> localWorkSize[total_kernel];
        std::pair<int, int> min_cost(INT_MAX, 0);//(min_time, min_index)
        for(int knl_idx = 0; knl_idx < actual_kernel; knl_idx++) {
            std::set<std::string> buildOption = mResource->mBuildOptions;
            if(outputShape.at(3) % itemC[knl_idx] != 0){
                buildOption.emplace("-DCHANNEL_BOUNDARY_PROTECT");
            }
            if((outputShape.at(2) % itemW[knl_idx]) != 0 || (outputShape.at(1) % itemH[knl_idx]) != 0){
                buildOption.emplace("-DBLOCK_LEAVE");
            }
            kernel[knl_idx]        = mOpenCLBackend->getOpenCLRuntime()->buildKernel("conv_2d_buf", kernelName[knl_idx], buildOption, mOpenCLBackend->getPrecision());
            uint32_t maxWorkGroupSize = static_cast<uint32_t>(mOpenCLBackend->getOpenCLRuntime()->getMaxWorkGroupSize(kernel[knl_idx]));

            int each_oc = (UP_DIV(outputShape.at(3), itemC[knl_idx]) + conv_block_num - 1) / conv_block_num;

            globalWorkSize[knl_idx] = {static_cast<uint32_t>(each_oc * UP_DIV(outputShape.at(2), itemW[knl_idx])), static_cast<uint32_t>(outputShape.at(0) * UP_DIV(outputShape.at(1), itemH[knl_idx]))};
            uint32_t idx            = 0;
            cl_int ret = CL_SUCCESS;
            ret |= kernel[knl_idx]->get().setArg(idx++, globalWorkSize[knl_idx][0]);
            ret |= kernel[knl_idx]->get().setArg(idx++, globalWorkSize[knl_idx][1]);
            ret |= kernel[knl_idx]->get().setArg(idx++, openCLBuffer(input));
            ret |= kernel[knl_idx]->get().setArg(idx++, openCLBuffer(mResource->mFilter.get()));
            ret |= kernel[knl_idx]->get().setArg(idx++, openCLBuffer(mResource->mBias.get()));
            ret |= kernel[knl_idx]->get().setArg(idx++, openCLBuffer(output));
            ret |= kernel[knl_idx]->get().setArg(idx++, sizeof(inputImageShape), inputImageShape);
            ret |= kernel[knl_idx]->get().setArg(idx++, inputChannels);
            ret |= kernel[knl_idx]->get().setArg(idx++, inputChannelBlocks);
            ret |= kernel[knl_idx]->get().setArg(idx++, batch);
            ret |= kernel[knl_idx]->get().setArg(idx++, sizeof(outputImageShape), outputImageShape);
            ret |= kernel[knl_idx]->get().setArg(idx++, sizeof(kernelShape), kernelShape);
            ret |= kernel[knl_idx]->get().setArg(idx++, sizeof(strideShape), strideShape);
            ret |= kernel[knl_idx]->get().setArg(idx++, sizeof(paddingShape), paddingShape);
            ret |= kernel[knl_idx]->get().setArg(idx++, sizeof(dilationShape), dilationShape);
            ret |= kernel[knl_idx]->get().setArg(idx++, UP_DIV(width, itemW[knl_idx]));
            ret |= kernel[knl_idx]->get().setArg(idx++, outChannelBlocks);
            ret |= kernel[knl_idx]->get().setArg(idx++, UP_DIV(height, itemH[knl_idx]));
            int outChannelBase = 0;
            ret |= kernel[knl_idx]->get().setArg(idx++, outChannelBase);
            if(mResource->mPrelu){
                ret |= kernel[knl_idx]->get().setArg(idx++, openCLBuffer(mResource->mSlope.get()));
            }
            MNN_CHECK_CL_SUCCESS(ret, "setArg ConvBuf Kernel Select");

            std::pair<std::vector<uint32_t>, int> retTune;
            retTune = localWS2DDefault(globalWorkSize[knl_idx], maxWorkGroupSize, mOpenCLBackend->getOpenCLRuntime(), kernelName[knl_idx] + info, kernel[knl_idx], mOpenCLBackend->getCLTuneLevel(), "conv_2d_buf");

            if(min_cost.first > retTune.second) {
                min_cost.first = retTune.second;
                min_cost.second = knl_idx;
                mLocalWorkSize = {retTune.first[0], retTune.first[1]};
            }
        }
        int min_index  = min_cost.second;
        mGlobalWorkSize = {globalWorkSize[min_index][0], globalWorkSize[min_index][1]};

        std::set<std::string> buildOption = mResource->mBuildOptions;
        if(outputShape.at(3) % itemC[min_index] != 0){
            buildOption.emplace("-DCHANNEL_BOUNDARY_PROTECT");
        }
        if((outputShape.at(2) % itemW[min_index]) != 0 || (outputShape.at(1) % itemH[min_index]) != 0){
            buildOption.emplace("-DBLOCK_LEAVE");
        }
        
        for(int kernel_idx = 0; kernel_idx < conv_block_num; kernel_idx++) {
            mKernel[kernel_idx]        = mOpenCLBackend->getOpenCLRuntime()->buildKernel("conv_2d_buf", kernelName[min_index], buildOption, mOpenCLBackend->getPrecision());
            
            uint32_t idx            = 0;
            cl_int ret = CL_SUCCESS;
            
            ret |= mKernel[kernel_idx]->get().setArg(idx++, mGlobalWorkSize[0]);
            ret |= mKernel[kernel_idx]->get().setArg(idx++, mGlobalWorkSize[1]);
            ret |= mKernel[kernel_idx]->get().setArg(idx++, openCLBuffer(input));
            ret |= mKernel[kernel_idx]->get().setArg(idx++, openCLBuffer(mResource->mFilter.get()));
            ret |= mKernel[kernel_idx]->get().setArg(idx++, openCLBuffer(mResource->mBias.get()));
            ret |= mKernel[kernel_idx]->get().setArg(idx++, openCLBuffer(output));
            ret |= mKernel[kernel_idx]->get().setArg(idx++, sizeof(inputImageShape), inputImageShape);
            ret |= mKernel[kernel_idx]->get().setArg(idx++, inputChannels);
            ret |= mKernel[kernel_idx]->get().setArg(idx++, inputChannelBlocks);
            ret |= mKernel[kernel_idx]->get().setArg(idx++, batch);
            ret |= mKernel[kernel_idx]->get().setArg(idx++, sizeof(outputImageShape), outputImageShape);
            ret |= mKernel[kernel_idx]->get().setArg(idx++, sizeof(kernelShape), kernelShape);
            ret |= mKernel[kernel_idx]->get().setArg(idx++, sizeof(strideShape), strideShape);
            ret |= mKernel[kernel_idx]->get().setArg(idx++, sizeof(paddingShape), paddingShape);
            ret |= mKernel[kernel_idx]->get().setArg(idx++, sizeof(dilationShape), dilationShape);
            ret |= mKernel[kernel_idx]->get().setArg(idx++, UP_DIV(width, itemW[min_index]));
            ret |= mKernel[kernel_idx]->get().setArg(idx++, outChannelBlocks);
            ret |= mKernel[kernel_idx]->get().setArg(idx++, UP_DIV(height, itemH[min_index]));
            int outChannelBase = mGlobalWorkSize[0] / UP_DIV(width, itemW[min_index]) * kernel_idx;
            ret |= mKernel[kernel_idx]->get().setArg(idx++, outChannelBase);
            if(mResource->mPrelu){
                ret |= mKernel[kernel_idx]->get().setArg(idx++, openCLBuffer(mResource->mSlope.get()));
            }
            MNN_CHECK_CL_SUCCESS(ret, "setArg ConvBuf");
        }
    }
    if (inputs.size() > 1) {
        backend()->onReleaseBuffer(mResource->mFilter.get(), Backend::DYNAMIC);
    }
    if (mResource->mConv1x1Opt && mResource->mConv1x1Local){
        mOpenCLBackend->recordKernel3d(mKernel[0], mGlobalWorkSize, mLocalWorkSize);
    }else{
        for(int i = 0; i < mKernel.size(); i++) {
            mOpenCLBackend->recordKernel2d(mKernel[i], mGlobalWorkSize, mLocalWorkSize);
        }
        mGlobalWorkSize[0] = ROUND_UP(mGlobalWorkSize[0], std::max((uint32_t)1, mLocalWorkSize[0]));
        mGlobalWorkSize[1] = ROUND_UP(mGlobalWorkSize[1], std::max((uint32_t)1, mLocalWorkSize[1]));
    }
    mOpenCLBackend->endRecord(mRecording);
#ifdef LOG_VERBOSE
    MNN_PRINT("end ConvExecution onResize !\n");
#endif
    return NO_ERROR;
}

ErrorCode ConvBufExecution::onExecute(const std::vector<Tensor *> &inputs, const std::vector<Tensor *> &outputs) {
#ifdef LOG_VERBOSE
    MNN_PRINT("Start ConvExecution onExecute !\n");
#endif
    if (inputs.size() > 1) {
        mResource->mRasterExe->onExecute({}, {mResource->mFilter.get()});
        if (inputs.size() > 2) {
            auto buffer_size = inputs[2]->elementSize();
            if(mOpenCLBackend->getPrecision() != BackendConfig::Precision_High) {
                buffer_size *= sizeof(half_float::half);
            } else {
                buffer_size *= sizeof(float);
            }
            mOpenCLBackend->getOpenCLRuntime()->commandQueue().enqueueCopyBuffer(openCLBuffer(inputs[2]), openCLBuffer(mResource->mBias.get()), 0, 0, buffer_size);
        }
    }
#ifdef ENABLE_OPENCL_TIME_PROFILER
    if (mPreKernel) {
        cl::Event event0;
        runKernel2D(mPreKernel, mPreGlobalWorkSize, mPreLocalWorkSize, mOpenCLBackend->getOpenCLRuntime(), &event0);
        mOpenCLBackend->getOpenCLRuntime()->pushEvent({"ConvBuf2D-gemm2-0", event0});
    }

    if(mResource->mConvGemmOptLevel == 1) {
        mStrassenComputor->onExecute();
    } else {
        cl::Event event;
        if (mResource->mConv1x1Opt && mResource->mConv1x1Local){
            run3DKernelDefault(mKernel[0], mGlobalWorkSize, mLocalWorkSize, mOpenCLBackend->getOpenCLRuntime(), &event);
        } else{
            runKernel2D(mKernel[0], mGlobalWorkSize, mLocalWorkSize, mOpenCLBackend->getOpenCLRuntime(), &event);
        }
        std::string name = "ConvBuf2D";
        std::string b = std::to_string(inputs[0]->batch());
        std::string ci = std::to_string(inputs[0]->channel());
        std::string hi = std::to_string(inputs[0]->height());
        std::string wi = std::to_string(inputs[0]->width());
        std::string co = std::to_string(outputs[0]->channel());
        std::string ho = std::to_string(outputs[0]->height());
        std::string wo = std::to_string(outputs[0]->width());
        std::string kh = std::to_string(mResource->mKernelHeight);
        std::string kw = std::to_string(mResource->mKernelWidth);
        std::string total = std::to_string(1.0 / 1000000 * inputs[0]->batch() * inputs[0]->channel() * outputs[0]->channel() * outputs[0]->height() * outputs[0]->width() * mResource->mKernelHeight * mResource->mKernelWidth);
        if (mResource->mConvGemmOptLevel > 0) {
            std::string m = std::to_string(outputs[0]->width() * outputs[0]->height() * inputs[0]->batch());
            name += "-gemm";
            name += std::to_string(mResource->mConvGemmOptLevel) + "-m" + m + "n" + co + "k" + ci;
        } else if (mResource->mConv1x1Opt) {
            name += "-conv1x1";
            name += "-b" + b + "ci" + ci + "hi" + hi + "wi" + wi + "co" + co;
        } else {
            name += "-ori-b" + b + "ci" + ci + "hi" + hi + "wi" + wi + "co" + co+ "ho" + ho + "wo" + wo + "kh" + kh + "kw" + kw;
        }
        name += "-total:" + total + "*10^6";
        mOpenCLBackend->getOpenCLRuntime()->pushEvent({name.c_str(), event});
        for(int i = 1; i < mKernel.size(); i++) {
            cl::Event event;
            runKernel2D(mKernel[i], mGlobalWorkSize, mLocalWorkSize, mOpenCLBackend->getOpenCLRuntime(), &event);
            mOpenCLBackend->getOpenCLRuntime()->pushEvent({name.c_str(), event});
        }
    }
    if (mPostKernel) {
        cl::Event event2;
        runKernel2D(mPostKernel, mPostGlobalWorkSize, mPostLocalWorkSize, mOpenCLBackend->getOpenCLRuntime(), &event2);
        mOpenCLBackend->getOpenCLRuntime()->pushEvent({"ConvBuf2D-gemm2-2", event2});
    }
#else
    if(mOpenCLBackend->isUseRecordQueue()){
        mOpenCLBackend->addRecord(mRecording, mOpRecordUpdateInfo);
#ifdef LOG_VERBOSE
        MNN_PRINT("End ConvExecution onExecute... \n");
#endif
        return NO_ERROR;
    }
    if (mPreKernel) {
        runKernel2D(mPreKernel, mPreGlobalWorkSize, mPreLocalWorkSize, mOpenCLBackend->getOpenCLRuntime());
    }
    if(mResource->mConvGemmOptLevel == 1) {
        mStrassenComputor->onExecute();
    } else {
        if (mResource->mConv1x1Opt && mResource->mConv1x1Local){
            run3DKernelDefault(mKernel[0], mGlobalWorkSize, mLocalWorkSize, mOpenCLBackend->getOpenCLRuntime());
        } else{
            for(int i = 0; i < mKernel.size(); i++) {
                runKernel2D(mKernel[i], mGlobalWorkSize, mLocalWorkSize, mOpenCLBackend->getOpenCLRuntime());
            }
        }
    }
    if (mPostKernel) {
        runKernel2D(mPostKernel, mPostGlobalWorkSize, mPostLocalWorkSize, mOpenCLBackend->getOpenCLRuntime());
    }
#endif

#ifdef LOG_VERBOSE
    MNN_PRINT("end ConvExecution onExecute !\n");
#endif
    return NO_ERROR;
}

class ConvolutionBufCreator : public OpenCLBackend::Creator {
public:
    virtual ~ConvolutionBufCreator() = default;
    virtual Execution *onCreate(const std::vector<Tensor *> &inputs, const std::vector<Tensor *> &outputs,
                                const MNN::Op *op, Backend *backend) const override {
        auto conv2D  = op->main_as_Convolution2D();
        auto input   = inputs[0];
        auto output  = outputs[0];
        auto padding = ConvolutionCommon::convolutionPad(inputs[0], outputs[0], conv2D->common());
        std::vector<int> inputShape  = tensorShapeFormat(input);
        std::vector<int> outputShape = tensorShapeFormat(output);
        const int outputChannel         = outputShape.at(3);
        const int inputChannels = inputShape.at(3);

        if (nullptr != op->main_as_Convolution2D()->quanParameter()) {
            auto quan = op->main_as_Convolution2D()->quanParameter();
            if (1 == quan->type() || 2 == quan->type()) {
                if (quan->has_scaleInt()) {
                    // Don't support IDST-int8 because of error
                    return nullptr;
                }
            }
        }

        if(op->main_as_Convolution2D()->common()->group() > 1){
            // Don't support group > 1 now
            return nullptr;
        }

        if (inputs.size() > 1) {
            // Multi inputs
            for (int i = 0; i < inputs.size(); ++i) {
                TensorUtils::setTensorSupportPack(inputs[i], false);
            }
            for (int i = 0; i < outputs.size(); ++i) {
                TensorUtils::setTensorSupportPack(outputs[i], false);
            }
            return new ConvBufExecution(inputs, outputs, op, backend);
        }
        
#ifdef MNN_LOW_MEMORY
        if (static_cast<OpenCLBackend *>(backend)->getMemory() == BackendConfig::Memory_Low){
            auto conv2dParams = op->main_as_Convolution2D();
            if (conv2dParams->quanParameter() != nullptr) {
                if (((conv2dParams->quanParameter()->type() == 4) ||
                     (conv2dParams->quanParameter()->type() == 1) ||
                     (conv2dParams->quanParameter()->type() == 2))) {
                    if ((1 == conv2dParams->quanParameter()->type() || 2 == conv2dParams->quanParameter()->type()) && conv2dParams->quanParameter()->has_scaleInt()) {
                        // Don't support IDST-int8 because of error
                        return nullptr;
                    }
                    for (int i = 0; i < inputs.size(); ++i) {
                        TensorUtils::setTensorSupportPack(inputs[i], false);
                    }
                    for (int i = 0; i < outputs.size(); ++i) {
                        TensorUtils::setTensorSupportPack(outputs[i], false);
                    }
                    return new ConvBufLowMemoryExecution(inputs, outputs, op, backend);
                } else {
                    MNN_ERROR("OpenCL Conv buf low memory init error. For Opencl Backend, only support low memory mode of int8 or int4 dequantization currently.\n");
                    return nullptr;
                }
            }
        }
#endif

        if (ConvBufWinograd::valid(conv2D->common(), inputs[0], outputs[0], static_cast<OpenCLBackend *>(backend)->getOpenCLRuntime()->getGpuType() == INTEL)) {
#ifdef MNN_SUPPORT_INTEL_SUBGROUP
            if(static_cast<OpenCLBackend *>(backend)->getOpenCLRuntime()->isSupportedIntelSubgroup()){
                std::vector<int> inputShape = tensorShapeFormat(input);
                std::vector<int> outputShape = tensorShapeFormat(output);
                const int src_width = inputShape.at(2);
                const int dst_width = outputShape.at(2);
                int pad_right                = (UP_DIV(dst_width, 2) - 1) * 2 + 3 - padding.first - src_width + 1;
                TensorUtils::setTensorPad(input, padding.first, pad_right, 0, 0);
                TensorUtils::setTensorChannelPack(input, 16);
            }
#endif /* MNN_SUPPORT_INTEL_SUBGROUP */
            return new ConvBufWinograd(op, backend);
        }
#ifdef MNN_SUPPORT_INTEL_SUBGROUP
        if (static_cast<OpenCLBackend *>(backend)->getOpenCLRuntime()->isSupportedIntelSubgroup() && outputChannel >= 16) {
            if (inputChannels >= 16) {
                auto pads = ConvolutionCommon::convolutionPadFull(inputs[0], outputs[0], conv2D->common());
                TensorUtils::setTensorPad(inputs[0], std::get<0>(pads), std::get<2>(pads), 0, 0);
                TensorUtils::setTensorChannelPack(inputs[0], 16);
            }
            return new ConvSubgroupBuf(inputs, outputs, op, backend);
        }
#endif /* MNN_SUPPORT_INTEL_SUBGROUP */
        
        for (int i = 0; i < inputs.size(); ++i) {
            TensorUtils::setTensorSupportPack(inputs[i], false);
        }
        for (int i = 0; i < outputs.size(); ++i) {
            TensorUtils::setTensorSupportPack(outputs[i], false);
        }
        return new ConvBufExecution(inputs, outputs, op, backend);
    }
};

REGISTER_OPENCL_OP_CREATOR(ConvolutionBufCreator, OpType_Convolution, BUFFER);

} // namespace OpenCL
} // namespace MNN
#endif /* MNN_OPENCL_BUFFER_CLOSED */
