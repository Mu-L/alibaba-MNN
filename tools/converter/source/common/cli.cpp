//
//  cli.cpp
//  MNNConverter
//
//  Created by MNN on 2019/01/31.
//  Copyright © 2018, Alibaba Group Holding Limited
//

#include "cli.hpp"
#if defined(_MSC_VER)
#include <Windows.h>
#undef min
#undef max
#else
#include <unistd.h>
#endif
#include <google/protobuf/util/json_util.h>
#include "OpCount.hpp"
#include "cxxopts.hpp"
#include "config.hpp"
#include "logkit.h"
#include <MNN/MNNDefine.h>
#include "MNN_generated.h"
#include "PostConverter.hpp"
#include "addBizCode.hpp"
#include "caffeConverter.hpp"
#include "liteConverter.hpp"
#include "onnxConverter.hpp"
#include "tensorflowConverter.hpp"
#include "torchConverter.hpp"
#include "writeFb.hpp"
#include <MNN/expr/Expr.hpp>
#include <MNN/expr/Module.hpp>
#include <MNN/expr/ExprCreator.hpp>
#include "CommonUtils.hpp"
#include "PostConverter.hpp"
#include "Json2Flatbuffer.hpp"
#include <fstream>
#include <sstream>
#include <cmath>
#include "core/MemoryFormater.h"
modelConfig::~modelConfig() {
    if (nullptr != compressInfo) {
        delete compressInfo;
    }
}
namespace MNN {
using namespace MNN::Express;
static std::string _getDataType(const halide_type_t& type) {
    switch (type.code) {
        case halide_type_float:
            if (type.bits == 32) {
                return "float";
            }
            if (type.bits == 16) {
                return "half";
            }
            break;
        case halide_type_uint:
            if (type.bits == 32) {
                return "uint32";
            }
            if (type.bits == 16) {
                return "uint16";
            }
            if (type.bits == 8) {
                return "uint8";
            }
            break;
        case halide_type_int:
            if (type.bits == 32) {
                return "int32";
            }
            if (type.bits == 16) {
                return "int16";
            }
            if (type.bits == 8) {
                return "int8";
            }
            break;
        default:
            break;
    }
    return "Unknown";
}
static std::string _getFormatString(MNN::Express::Dimensionformat format) {
    switch (format) {
        case MNN::Express::NCHW:
            return "NCHW";
        case MNN::Express::NHWC:
            return "NHWC";
        case MNN::Express::NC4HW4:
            return "NC4HW4";
        default:
            break;
    }
    return "Unknown";
}
static int dumpModelInfo(const char* modelName) {
    std::vector<std::string> empty;
    std::shared_ptr<MNN::Express::Module> module(MNN::Express::Module::load(empty, empty, modelName));
    if (nullptr == module.get()) {
        MNN_ERROR("Load MNN from %s Failed\n", modelName);
        return 1;
    }
    auto info = module->getInfo();
    MNN_ASSERT(info->inputNames.size() == info->inputs.size());
    MNN_PRINT("Model default dimensionFormat is %s\n", _getFormatString(info->defaultFormat).c_str());
    MNN_PRINT("Model Inputs:\n");
    for (int i=0; i<info->inputNames.size(); ++i) {
        auto& varInfo = info->inputs[i];
        MNN_PRINT("[ %s ]: dimensionFormat: %s, ", info->inputNames[i].c_str(), _getFormatString(varInfo.order).c_str());
        MNN_PRINT("size: [ ");
        if (varInfo.dim.size() > 0) {
            for (int j=0; j<(int)varInfo.dim.size() - 1; ++j) {
                MNN_PRINT("%d,", varInfo.dim[j]);
            }
            MNN_PRINT("%d ", varInfo.dim[(int)varInfo.dim.size() - 1]);
        }
        MNN_PRINT("], ");
        MNN_PRINT("type is %s\n", _getDataType(varInfo.type).c_str());
    }
    MNN_PRINT("Model Outputs:\n");
    for (int i=0; i<info->outputNames.size(); ++i) {
        MNN_PRINT("[ %s ]\n", info->outputNames[i].c_str());
    }
    if (info->version.empty()) {
        MNN_PRINT("Model Version: < 2.0.0\n");
    } else {
        MNN_PRINT("Model Version: %s \n", info->version.c_str());
    }
    return 0;
}

bool Cli::initializeMNNConvertArgs(modelConfig &modelPath, int argc, char **argv) {
    cxxopts::Options options("MNNConvert");

    options.positional_help("[optional args]").show_positional_help();

    options.allow_unrecognised_options().add_options()(std::make_pair("h", "help"), "Convert Other Model Format To MNN Model\n")(
                                                                                                                                 std::make_pair("v", "version"), "show current version")
    (std::make_pair("f", "framework"),
#ifdef MNN_BUILD_TORCH
     "model type, ex: [TF,CAFFE,ONNX,TFLITE,MNN,TORCH,JSON]",
#else
     "model type, ex: [TF,CAFFE,ONNX,TFLITE,MNN,JSON]",
#endif
     cxxopts::value<std::string>())
    (
     "modelFile",
     "tensorflow Pb or caffeModel, ex: *.pb,*caffemodel",
     cxxopts::value<std::string>()
     )
    (
     "batch",
     "if model input's batch is not set, set as the batch size you set",
     cxxopts::value<int>()
     )
    (
     "keepInputFormat",
     "keep input dimension format or not, default: true",
     cxxopts::value<bool>()
     )
    (
     "optimizeLevel",
     "graph optimize option, 0: don't run optimize(only support for MNN source), 1: use graph optimize only for every input case is right, 2: normally right but some case may be wrong, default 1",
     cxxopts::value<int>()
     )
    (
     "optimizePrefer",
     "graph optimize option, 0 for normal, 1 for smalleset, 2 for fastest",
     cxxopts::value<int>()
     )
    (
     "prototxt",
     "only used for caffe, ex: *.prototxt",
     cxxopts::value<std::string>())
    (
     "MNNModel",
     "MNN model, ex: *.mnn",
     cxxopts::value<std::string>())
    (
     "fp16",
     "save Conv's weight/bias in half_float data type")
    (
     "benchmarkModel",
     "Do NOT save big size data, such as Conv's weight,BN's gamma,beta,mean and variance etc. Only used to test the cost of the model")
    (
     "bizCode",
     "MNN Model Flag, ex: MNN",
     cxxopts::value<std::string>())
    (
     "debug",
     "Enable debugging mode."
     )
    (
     "forTraining",
     "whether or not to save training ops BN and Dropout, default: false",
     cxxopts::value<bool>()
     )
    (
     "weightQuantBits",
     "save conv/matmul/LSTM float weights to int8 type, only optimize for model size, 2-8 bits, default: 0, which means no weight quant",
     cxxopts::value<int>()
     )
    (
     "weightQuantAsymmetric",
     "the default weight-quant uses SYMMETRIC quant method, which is compatible with old MNN versions. "
     "you can try set --weightQuantAsymmetric to use asymmetric quant method to improve accuracy of the weight-quant model in some cases, "
     "but asymmetric quant model cannot run on old MNN versions. You will need to upgrade MNN to new version to solve this problem. default: false",
     cxxopts::value<bool>()
     )
    (
     "weightQuantBlock",
     "using block-wise weight quant, set block size, defaut: -1, which means channel-wise weight quant",
     cxxopts::value<int>()
     )
    (
     "compressionParamsFile",
     "The path of the compression parameters that stores activation, "
     "weight scales and zero points for quantization or information "
     "for sparsity. "
     "if the file does not exist, will create file base on user's option",
     cxxopts::value<std::string>()
     )
    (
     "OP",
     "print framework supported op",
     cxxopts::value<bool>()
     )
    (
     "saveStaticModel",
     "save static model with fix shape, default: false",
     cxxopts::value<bool>()
     )
    (
     "targetVersion",
     "compability for old mnn engine, default the same as converter",
     cxxopts::value<float>()
     )
    (
     "customOpLibs",
     "custom op libs ex: libmy_add.so;libmy_sub.so",
     cxxopts::value<std::string>()
     )
    (
     "info",
     "dump MNN's model info"
     )
    (
     "authCode",
     "code for model authentication.",
     cxxopts::value<std::string>()
     )
    (
     "inputConfigFile",
     "set input config file for static model, ex: ~/config.txt",
     cxxopts::value<std::string>()
     )
    (
     "testdir",
     "set test dir, mnn will convert model and then check the result",
     cxxopts::value<std::string>()
     )
    (
     "testconfig",
     "set test config json, example: tools/converter/forward.json",
     cxxopts::value<std::string>()
     )
    (
     "thredhold",
     "if set test dir, thredhold mean the max rate permit for run MNN model and origin error",
     cxxopts::value<float>()
     )
    (
     "JsonFile",
     "if input model is MNN and give jsonfile, while Dump MNN model to the JsonFile.",
     cxxopts::value<std::string>()
     )
    (
     "alignDenormalizedValue",
     "if 1, converter would align denormalized float(|x| < 1.18e-38) as zero, because of in ubuntu/protobuf or android/flatbuf, system behaviors are different. default: 1, range: {0, 1}",
     cxxopts::value<int>()
     )
    (
     "detectSparseSpeedUp",
     "if add the flag converter would detect weights sparsity and check sparse speedup, may decrease model size, but will cause more time for convert."
     )
    (
     "saveExternalData",
     "save weight to extenal bin file.",
     cxxopts::value<bool>()
     )
    (
     "useGeluApproximation",
     "Use Gelu Approximation Compute Instead of use ERF",
     cxxopts::value<int>()
     )
    (
     "convertMatmulToConv",
     "if 1, converter matmul with constant input to convolution. default: 1, range: {0, 1}",
     cxxopts::value<int>()
     )
    (
     "transformerFuse",
     "fuse key transformer op, like attention. default: false",
     cxxopts::value<bool>()
     )
    (
     "groupConvNative",
     "keep native group convolution. default: false",
     cxxopts::value<bool>()
     )
     (
     "allowCustomOp",
     "allow custom op when convert. default: false",
     cxxopts::value<bool>()
     )
     (
      "useOriginRNNImpl",
      "Don't use While Module to Implement LSTM or GRU, use origin OP, if open it, LSTM and GRU can't be quantized or use other compress method",
      cxxopts::value<bool>()
     )
    ;

    auto result = options.parse(argc, argv);

    if (result.count("help")) {
        std::cout << options.help({""}) << std::endl;
        return false;
    }

    if (result.count("version")) {
        std::cout << MNN_VERSION << std::endl;
        return false;
    }

    modelPath.model = modelPath.MAX_SOURCE;
    // model source
    std::string frameWork;
    if (result.count("framework")) {
        frameWork = result["framework"].as<std::string>();
        if ("TF" == frameWork) {
            modelPath.model = modelConfig::TENSORFLOW;
        } else if ("CAFFE" == frameWork) {
            modelPath.model = modelConfig::CAFFE;
        } else if ("ONNX" == frameWork) {
            modelPath.model = modelConfig::ONNX;
        } else if ("MNN" == frameWork) {
            modelPath.model = modelConfig::MNN;
        } else if ("TFLITE" == frameWork) {
            modelPath.model = modelConfig::TFLITE;
#ifdef MNN_BUILD_TORCH
        } else if ("TORCH" == frameWork) {
            modelPath.model = modelConfig::TORCH;
#endif
        } else if ("JSON" == frameWork) {
            modelPath.model = modelConfig::JSON;
        } else {
            std::cout << "Framework Input ERROR or Not Support This Model Type Now!" << std::endl;
            return false;
        }
    } else {
        std::cout << options.help({""}) << std::endl;
        DLOG(INFO) << "framework Invalid, use -f CAFFE/MNN/ONNX/TFLITE/TORCH/JSON !";
        return false;
    }
    if (result.count("OP")) {
        MNN_PRINT("Dump %s support Ops\n", frameWork.c_str());
        const auto& res = OpCount::get()->getMap().find(frameWork);
        if (res == OpCount::get()->getMap().end()) {
            return false;
        }
        for (const auto& iter : res->second) {
            MNN_PRINT("%s\n", iter.c_str());
        }
        MNN_PRINT("Total: %d\n", (int)res->second.size());
        return false;
    }

    // model file path
    if (result.count("modelFile")) {
        const std::string modelFile = result["modelFile"].as<std::string>();
        if (CommonKit::FileIsExist(modelFile)) {
            modelPath.modelFile = modelFile;
        } else {
            DLOG(INFO) << "Model File Does Not Exist! ==> " << modelFile;
            return false;
        }
    } else {
        DLOG(INFO) << "modelFile Not set Invalid, use --modelFile to set!";
        return false;
    }
    // Optimize Level
    if (result.count("optimizeLevel")) {
        modelPath.optimizeLevel = result["optimizeLevel"].as<int>();
        if (modelPath.optimizeLevel > 1) {
            DLOG(INFO) << "\n optimizeLevel > 1, some case may be wrong";
        }
    }

    // prototxt file path
    if (result.count("prototxt")) {
        const std::string prototxt = result["prototxt"].as<std::string>();
        if (CommonKit::FileIsExist(prototxt)) {
            modelPath.prototxtFile = prototxt;
        } else {
            DLOG(INFO) << "Proto File Does Not Exist!";
            return false;
        }
    } else {
        // caffe model must have this option
        if (modelPath.model == modelPath.CAFFE) {
            DLOG(INFO) << "Proto File Not Set, use --prototxt XXX.prototxt to set it!";
            return false;
        }
    }

    // MNN model output path
    if (result.count("MNNModel")) {
        const std::string MNNModelPath = result["MNNModel"].as<std::string>();
        modelPath.MNNModel             = MNNModelPath;
    } else if (result.count("JsonFile")) {
        const std::string JsonFilePath = result["JsonFile"].as<std::string>();
        modelPath.mnn2json             = true;
        modelPath.MNNModel             = JsonFilePath;
    } else if (result.count("info") && modelPath.model == modelConfig::MNN) {
        modelPath.dumpInfo = true;
        return true;
    } else {
        DLOG(INFO) << "MNNModel File Not Set, use --MNNModel XXX.prototxt to set it!";
        return false;
    }
    if (result.count("targetVersion")) {
        auto version = result["targetVersion"].as<float>();
        std::cout << "TargetVersion is " << version << std::endl;
        modelPath.targetVersion = version;
    }
    // add MNN bizCode
    if (result.count("bizCode")) {
        const std::string bizCode = result["bizCode"].as<std::string>();
        modelPath.bizCode         = bizCode;
    } else {
        MNN_ERROR("Don't has bizCode, use MNNTest for default\n");
        modelPath.bizCode = "MNNTest";
    }

    // input config file path
    if (result.count("inputConfigFile")) {
        const std::string inputConfigFile = result["inputConfigFile"].as<std::string>();
        modelPath.inputConfigFile         = inputConfigFile;
    }

    // half float
    if (result.count("fp16")) {
        modelPath.saveHalfFloat = true;
    }
    if (result.count("forTraining")) {
        modelPath.forTraining = true;
    }
    if (result.count("batch")) {
        modelPath.defaultBatchSize = result["batch"].as<int>();
    }
    if (result.count("keepInputFormat")) {
        modelPath.keepInputFormat = result["keepInputFormat"].as<bool>();
    }
    if (result.count("weightQuantBits")) {
        modelPath.weightQuantBits = result["weightQuantBits"].as<int>();
    }
    if (result.count("weightQuantAsymmetric")) {
        modelPath.weightQuantAsymmetric = result["weightQuantAsymmetric"].as<bool>();
    }
    if (result.count("weightQuantBlock")) {
        modelPath.weightQuantBlock = result["weightQuantBlock"].as<int>();
    }
    if (result.count("saveStaticModel")) {
        modelPath.saveStaticModel = true;
    }
    if (result.count("optimizePrefer")) {
        modelPath.optimizePrefer = result["optimizePrefer"].as<int>();
    }
    // Int8 calibration table path.
    if (result.count("compressionParamsFile")) {
        modelPath.compressionParamsFile =
        result["compressionParamsFile"].as<std::string>();
    }
    if (result.count("customOpLibs")) {
        modelPath.customOpLibs = result["customOpLibs"].as<std::string>();
    }
    if (result.count("authCode")) {
        modelPath.authCode = result["authCode"].as<std::string>();
    }
    if (result.count("alignDenormalizedValue")) {
        modelPath.alignDenormalizedValue = result["alignDenormalizedValue"].as<int>();
    }
    if (result.count("detectSparseSpeedUp")) {
        modelPath.detectSparseSpeedUp = true;
    }
    if (result.count("convertMatmulToConv")) {
        modelPath.convertMatmulToConv = result["convertMatmulToConv"].as<int>();
    }
    if (result.count("useGeluApproximation")) {
        modelPath.useGeluApproximation = result["useGeluApproximation"].as<int>();
    }
    if (result.count("testdir")) {
        modelPath.testDir = result["testdir"].as<std::string>();
    }
    if (result.count("testconfig")) {
        modelPath.testConfig = result["testconfig"].as<std::string>();
    }
    if (result.count("thredhold")) {
        modelPath.testThredhold = result["thredhold"].as<float>();
    }
    if (result.count("saveExternalData")) {
        modelPath.saveExternalData = true;
    }
    if (result.count("transformerFuse")) {
        modelPath.transformerFuse = true;
    }
    if (result.count("groupConvNative")) {
        modelPath.groupConvNative = true;
    }
    if (result.count("allowCustomOp")) {
        modelPath.allowCustomOp = true;
    }
    if (result.count("useOriginRNNImpl")) {
        modelPath.useOriginRNNImpl = true;
    }
    return true;
}

static void computeUnaryBuffer(MNN::NetT* net) {
    for (auto iter = net->oplists.begin(); iter != net->oplists.end(); ++iter) {
        auto op = iter->get();
        auto opType = op->type;
        std::map<int, TensorDescribeT*> describes;
        for (auto& des : net->extraTensorDescribe) {
            describes.insert(std::make_pair(des->index, des.get()));
        }
        if (opType == MNN::OpType_Sigmoid || opType == MNN::OpType_TanH) {
            op->type = OpType_UnaryOp;
            op->main.value = new UnaryOpT;
            op->main.type = OpParameter_UnaryOp;
            op->main.AsUnaryOp()->opType = UnaryOpOperation_SIGMOID;
            if (opType == MNN::OpType_TanH) {
                op->main.AsUnaryOp()->opType = UnaryOpOperation_TANH;
            }
            opType = op->type;
        }
        if (opType == MNN::OpType_UnaryOp) {
            auto type = op->main.AsUnaryOp()->opType;
            if (type == UnaryOpOperation_ABS || type == UnaryOpOperation_NEG || type == UnaryOpOperation_SIGN) {
                continue;
            }
            op->main.AsUnaryOp()->tableInt8.resize(255);
            auto unaryParam = op->main.AsUnaryOp()->tableInt8.data();

            auto outputId = op->outputIndexes[0];
            if (describes.find(outputId) == describes.end()) {
                continue;
            }
            auto unaryDes = describes.find(outputId)->second;
            float outScale = unaryDes->quantInfo->scale;
            float outZero  = unaryDes->quantInfo->zero;
            auto inputId = op->inputIndexes[0];
            if (describes.find(inputId) == describes.end()) {
                auto iter = describes.find(outputId);

            }
            unaryDes = describes.find(inputId)->second;
            float inpScale = unaryDes->quantInfo->scale;
            float inpZero  = unaryDes->quantInfo->zero;

            // Read input data.
            std::vector<float> dataInput;
            float fx = 0.f;
            auto input = _Input({255}, NCHW, halide_type_of<float>());
            input->setName("input_tensor");
            auto ptr_in = input->template writeMap<float>();
            for (int i = -127; i <= 127; ++i) {
                fx = (i - inpZero) * inpScale;
                dataInput.push_back(fx);
                ptr_in[i + 127] = fx;
            }
            input->unMap();
            auto output = Variable::create(Expr::create(op, {input}));
            auto gotOutput = output->template readMap<float>();
            // Write output data.
            int val;
            for (int i = 0; i < 255; ++i) {
                val = (int)roundf(gotOutput[i] / outScale) + outZero;
                if (val > 127) {
                    val = 127;
                }
                if (val < -127) {
                    val = -127;
                }
                unaryParam[i] = val;
            }
        }
    }
}
static void _reorderInputs(const std::vector<std::string>& inputNames, MNN::NetT* netT) {
    if (!inputNames.empty()) {
        // Make Input op order the same as origin model
        auto oplists = std::move(netT->oplists);
        std::vector<std::unique_ptr<MNN::OpT>> inputOps;
        for (auto& op : oplists) {
            if (nullptr == op.get()) {
                continue;
            }
            if (op->type != MNN::OpType_Input || op->outputIndexes.empty()) {
                continue;
            }
            inputOps.emplace_back(std::move(op));
        }

        for (int i=0; i<inputNames.size(); ++i) {
            for (auto& op : inputOps) {
                if (nullptr == op.get()) {
                    // Has used
                    continue;
                }
                if (netT->tensorName[op->outputIndexes[0]] == inputNames[i]) {
                    netT->oplists.emplace_back(std::move(op));
                    break;
                }
            }
        }
        for (auto& op : oplists) {
            if (nullptr != op.get()) {
                netT->oplists.emplace_back(std::move(op));
            }
        }
    }
}

bool Cli::convertModel(modelConfig& modelPath) {
    if (modelPath.dumpInfo) {
        dumpModelInfo(modelPath.modelFile.c_str());
        return true;
    }
    std::cout << "Start to Convert Other Model Format To MNN Model..., target version: " << modelPath.targetVersion << std::endl;
    std::unique_ptr<MNN::NetT> netT = std::unique_ptr<MNN::NetT>(new MNN::NetT());
    int parseRes = 1;
    std::unique_ptr<MNN::OpT> metaOp(new MNN::OpT);
    metaOp->type = MNN::OpType_Extra;
    metaOp->main.value = new MNN::ExtraT;
    metaOp->main.type = MNN::OpParameter_Extra;
    metaOp->main.AsExtra()->type = "Meta";
    metaOp->main.AsExtra()->engine = "MNN";
    std::vector<std::string> inputNames;
    if (modelPath.model == modelConfig::CAFFE) {
        parseRes = caffe2MNNNet(modelPath.prototxtFile, modelPath.modelFile, modelPath.bizCode, netT);
    } else if (modelPath.model == modelConfig::TENSORFLOW) {
        parseRes = tensorflow2MNNNet(modelPath.modelFile, modelPath.bizCode, netT);
    } else if (modelPath.model == modelConfig::MNN) {
        if (modelPath.mnn2json) {
            if (mnn2json(modelPath.modelFile.c_str(), modelPath.MNNModel.c_str())) {
                MNN_PRINT("MNNModel %s has convert to JsonFile %s.\n", modelPath.modelFile.c_str(), modelPath.MNNModel.c_str());
                return true;
            } else {
                MNN_ERROR("[ERROR] MNN to Json failed.\n");
                return false;
            }
        } else {
            parseRes = addBizCode(modelPath.modelFile, modelPath.bizCode, netT);
        }
    } else if (modelPath.model == modelConfig::ONNX) {
        parseRes = onnx2MNNNet(modelPath.modelFile, modelPath.bizCode, netT, metaOp.get(), inputNames);
    } else if (modelPath.model == modelConfig::TFLITE) {
        parseRes = tflite2MNNNet(modelPath.modelFile, modelPath.bizCode, netT);
#ifdef MNN_BUILD_TORCH
    } else if (modelPath.model == modelConfig::TORCH) {
        parseRes = torch2MNNNet(modelPath.modelFile, modelPath.bizCode, netT, modelPath.customOpLibs);
#endif
    } else if (modelPath.model == modelConfig::JSON) {
        if (json2mnn(modelPath.modelFile.c_str(), modelPath.MNNModel.c_str())) {
            MNN_PRINT("JsonFile %s has convert to MNNModel %s.\n", modelPath.modelFile.c_str(), modelPath.MNNModel.c_str());
            return true;
        } else {
            MNN_ERROR("[ERROR] Json to MNN failed.\n");
            return false;
        }
    } else {
        MNN_ERROR("[ERROR] Not Support Model Type.\n");
    }
    if (netT.get() == nullptr || parseRes) {
        MNN_ERROR("[ERROR] Convert error, please check your file format.\n");
        return false;
    }
    int error = 0;
    if (modelPath.defaultBatchSize > 0) {
        for (const auto& op : netT->oplists) {
            if (op->type != OpType_Input || nullptr == op->main.AsInput()) {
                continue;
            }
            auto inputP = op->main.AsInput();
            if (inputP->dims.size() >= 1 && inputP->dims[0] <= 0) {
                std::cout << "Set " << op->name << " batch = " << modelPath.defaultBatchSize << std::endl;
                inputP->dims[0] = modelPath.defaultBatchSize;
            }
        }
    }
    bool needOptimize = modelPath.model != modelConfig::MNN || modelPath.optimizeLevel >= 1;
    if (modelPath.saveStaticModel && modelPath.model == modelConfig::MNN) {
        MNN_PRINT("Skip Optimize for static model\n");
        needOptimize = false;
    }
    std::vector<std::string> expectedPass;
    if (1 == modelPath.optimizeLevel && modelPath.model == modelConfig::MNN) {
        expectedPass = {
            "TranslateJsonOp",
            "FuseDupOp"
        };
    }
    CommonKit::loadCompress(modelPath);
    if (needOptimize) {
        std::cout << "Start to Optimize the MNN Net..." << std::endl;
        std::unique_ptr<MNN::NetT> newNet = optimizeNet(netT, modelPath.forTraining, modelPath, expectedPass);
        if (newNet->extraTensorDescribe.size()>0) {
            MNN_PRINT("MNN net has tensor quant info\n");
            computeUnaryBuffer(newNet.get());
        }
        _reorderInputs(inputNames, newNet.get());
        error = writeFb(newNet, modelPath, std::move(metaOp));
    } else {
        _reorderInputs(inputNames, netT.get());
        error = writeFb(netT, modelPath, std::move(metaOp));
    }
    if (0 == error) {
        std::cout << "Converted Success!" << std::endl;
    } else {
        std::cout << "Converted Failed!" << std::endl;
    }
    if (modelPath.testDir.size() > 0) {
        std::cout << "Check convert result by " << modelPath.testDir << ", thredhold is " << modelPath.testThredhold << std::endl;
        Cli::testconvert(modelPath.MNNModel, modelPath.testDir, modelPath.testThredhold, modelPath.testConfig);
    }
    return true;
}

static bool compareOutput(MNN::Express::VARP output, const std::string& directName, const std::string& name, MNN::Express::Dimensionformat dataFormat, int order, float maxError) {
    auto info = output->getInfo();
    auto ptr = output->readMap<float>();
    if (info && info->size <= 0) {
        MNN_PRINT("skip checking value for zero content tensor %s\n", name.c_str());
        return true;
    }

    if (nullptr == info || nullptr == ptr) {
        MNN_ERROR("TESTERROR name:%s, info:%p, ptr:%p.\n", name.c_str(), info, ptr);
        return false;
    }
    std::ifstream outputOrigin;
    // First find key
    {
        std::ostringstream outputFileOs;
        outputFileOs << directName << "/" << name <<".txt";
        outputOrigin.open(outputFileOs.str().c_str());
    }
    // Second find order
    if (outputOrigin.fail()) {
        std::ostringstream outputFileOs;
        outputFileOs << directName << "/" << order <<".txt";
        outputOrigin.open(outputFileOs.str().c_str());
    }
    if (outputOrigin.fail()) {
        MNN_PRINT("Skip check %s\n", name.c_str());
        return true;
    }
    if (info->order == MNN::Express::NC4HW4 && info->dim.size() > 1) {
        output = _Convert(output, dataFormat);
        info = output->getInfo();
    }
    if (info->type.code != halide_type_float) {
        output = MNN::Express::_Cast<float>(output);
        info = output->getInfo();
    }
    MNN_PRINT("%s: (", name.c_str());
    for (int i=0; i<info->dim.size(); ++i) {
        MNN_PRINT("%d, ", info->dim[i]);
    }
    MNN_PRINT(")\n");
    auto targetValue = _Input({info->dim}, info->order, info->type);
    auto targetPtr = targetValue->writeMap<float>();
    for (int i=0; i<info->size; ++i) {
        double tempValue;
        outputOrigin >> tempValue;
        targetPtr[i] = tempValue;
    }

    auto absMax = MNN::Express::_ReduceMax(MNN::Express::_Abs(targetValue), {});
    absMax = MNN::Express::_Maximum(absMax, MNN::Express::_Scalar<float>(0.0001f));
    auto diff = MNN::Express::_Abs(targetValue - output);
    auto outputPtr = output->readMap<float>();
#define MNN_IS_INF(x) (fabs(x) == INFINITY)
#define MNN_IS_NAN(x) ((x) != (x))
    for (int i=0; i<info->size; ++i) {
        if (MNN_IS_INF(outputPtr[i]) || MNN_IS_NAN(outputPtr[i])) {
            MNN_ERROR("TESTERROR %s value error:%f\n", name.c_str(), outputPtr[i]);
            return false;
        }
    }
    auto diffAbsMax = MNN::Express::_ReduceMax(diff);
    auto absMaxV = absMax->readMap<float>()[0];
    auto diffAbsMaxV = diffAbsMax->readMap<float>()[0];
    if (absMaxV * maxError < diffAbsMaxV || std::isnan(absMaxV)) {
        MNN_ERROR("TESTERROR %s value error : absMaxV:%f - DiffMax %f\n", name.c_str(), absMaxV, diffAbsMaxV);
        return false;
    }
    return true;
}

int Cli::testconvert(const std::string& defaultCacheFile, const std::string& directName, float maxErrorRate, const std::string& backendConfigJson) {
    std::map<std::string, float> inputInfo;
    std::map<std::string, std::vector<int>> inputShape;
    std::vector<std::string> inputNames;
    std::vector<std::string> outputNames;
    {
        rapidjson::Document document;
        std::ostringstream jsonNameOs;
        jsonNameOs << directName << "/input.json";
        std::ifstream fileNames(jsonNameOs.str().c_str());
        std::ostringstream output;
        output << fileNames.rdbuf();
        auto outputStr = output.str();
        document.Parse(outputStr.c_str());
        if (document.HasParseError()) {
            MNN_ERROR("Invalid json\n");
            return 0;
        }
        if (document.HasMember("inputs")) {
            auto inputsInfo = document["inputs"].GetArray();
            for (auto iter = inputsInfo.begin(); iter !=inputsInfo.end(); iter++) {
                auto obj = iter->GetObject();
                std::string name = obj["name"].GetString();
                inputNames.emplace_back(name);
                MNN_PRINT("%s\n", name.c_str());
                if (obj.HasMember("value")) {
                    float value = obj["value"].GetFloat();
                    inputInfo.insert(std::make_pair(name, value));
                }
                if (obj.HasMember("shape")) {
                    auto dims = obj["shape"].GetArray();
                    std::vector<int> shapes;
                    for (auto iter = dims.begin(); iter != dims.end(); iter++) {
                        shapes.emplace_back(iter->GetInt());
                    }
                    inputShape.insert(std::make_pair(name, shapes));
                }
            }
        }
        if (document.HasMember("outputs")) {
            auto array = document["outputs"].GetArray();
            for (auto iter = array.begin(); iter !=array.end(); iter++) {
                std::string name = iter->GetString();
                MNN_PRINT("output: %s\n", name.c_str());
                outputNames.emplace_back(name);
            }
        }
    }

    // create session
    MNN::ScheduleConfig config;
    config.type      = MNN_FORWARD_CPU;
    /*modeNum means gpuMode for GPU usage, Or means numThread for CPU usage.*/
    config.numThread = 2;
    // If type not fount, let it failed
    config.backupType = MNN_FORWARD_CPU;
    BackendConfig backendConfig;
    backendConfig.precision = static_cast<MNN::BackendConfig::PrecisionMode>(1);
    config.backendConfig     = &backendConfig;
    std::vector<int> hints;

    if (!backendConfigJson.empty()) {
        do {
            rapidjson::Document configDoc;
            std::ifstream configOs(backendConfigJson.c_str());
            if (configOs.fail()) {
                break;
            }
            std::ostringstream outputConfigOs;
            outputConfigOs << configOs.rdbuf();
            auto outputStr = outputConfigOs.str();
            configDoc.Parse(outputStr.c_str());
            if (configDoc.HasParseError()) {
                MNN_ERROR("Invalid json for backend config\n");
                break;
            }
            if (configDoc.HasMember("backend")) {
                config.type = (MNNForwardType)configDoc["backend"].GetInt();
            }
            if (configDoc.HasMember("mode")) {
                config.mode = configDoc["mode"].GetInt();
            }
            if (configDoc.HasMember("precision")) {
                config.backendConfig->precision = (MNN::BackendConfig::PrecisionMode)configDoc["precision"].GetInt();
            }
            if (configDoc.HasMember("memory")) {
                config.backendConfig->memory = (MNN::BackendConfig::MemoryMode)configDoc["memory"].GetInt();
            }
            if (configDoc.HasMember("power")) {
                config.backendConfig->power = (MNN::BackendConfig::PowerMode)configDoc["power"].GetInt();
            }
            if (configDoc.HasMember("hints")) {
                auto array = configDoc["hints"].GetArray();
                for (auto iter = array.Begin(); iter != array.End(); iter++) {
                    hints.emplace_back(iter->GetInt());
                }
                if (hints.size() % 2 != 0) {
                    MNN_ERROR("Invalid hint number: %d\n", hints.size());
                }
            }
        } while (false);
    }

    MNN::Express::Module::Config mConfig;
    mConfig.shapeMutable = true;
    std::shared_ptr<MNN::Express::Executor::RuntimeManager> rtmgr(MNN::Express::Executor::RuntimeManager::createRuntimeManager(config));
    for (int v=0; v<hints.size()/2; ++v) {
        rtmgr->setHint((Interpreter::HintMode)hints[2*v], hints[2*v+1]);
    }
    rtmgr->setHint(MNN::Interpreter::INIT_THREAD_NUMBER, 2);

    rtmgr->setExternalFile("./convert_cache.mnn.weight");
    std::shared_ptr<MNN::Express::Module> net(MNN::Express::Module::load(inputNames, outputNames, defaultCacheFile.c_str(), rtmgr, &mConfig));
    std::shared_ptr<MNN::Express::Module> net2;
    net2.reset(MNN::Express::Module::clone(net.get()));
    net = net2;
    auto mInfo = net->getInfo();
    std::vector<MNN::Express::VARP> inputs(mInfo->inputs.size());
#define LOAD_DATA(TYPE)\
    if (inputInfo.find(inputName) != inputInfo.end()) {\
        auto value = inputInfo[inputName];\
        for (int i=0; i<info->size; ++i) {\
            ptr[i] = value;\
        }\
    } else {\
        std::ostringstream fileNameOs;\
        fileNameOs << directName << "/" << inputName << ".txt";\
        auto fileName = fileNameOs.str();\
        std::ifstream inputOs(fileName.c_str());\
        if (inputOs.fail()) {\
            MNN_ERROR("TESTERROR Can't open %s\n", fileName.c_str());\
            continue;\
        }\
        for (int i=0; i<info->size; ++i) {\
            double tempValue;inputOs >> tempValue;\
            ptr[i] = tempValue;\
        }\
    }
    // Load inputs
    for (int i=0; i<inputs.size(); ++i) {
        auto inputName = inputNames[i];
        // Resize
        auto shapeIter = inputShape.find(inputName);
        if (shapeIter != inputShape.end()) {
            auto s = shapeIter->second;
            inputs[i] = _Input(s, mInfo->defaultFormat, mInfo->inputs[i].type);
        } else {
            inputs[i] = _Input(mInfo->inputs[i].dim, mInfo->inputs[i].order, mInfo->inputs[i].type);
        }
        auto info = inputs[i]->getInfo();
        auto iter = inputInfo.find(inputNames[i]);
        if (iter != inputInfo.end()) {
            auto ptr = inputs[i]->writeMap<float>();
            for (int v=0; v<mInfo->inputs[i].size; ++v) {
                ptr[v] = iter->second;
            }
            continue;
        }
        if (info->type == halide_type_of<float>()){
            auto ptr = inputs[i]->writeMap<float>();
            LOAD_DATA(float)
        } else {
            auto floatVar = _Input(info->dim, info->order, halide_type_of<float>());
            auto ptr = floatVar->writeMap<float>();
            LOAD_DATA(float)
            auto temp = _Cast(floatVar, info->type);
            inputs[i]->input(temp);
        }
        inputs[i] = _Convert(inputs[i], mInfo->inputs[i].order);
    }
#undef LOAD_DATA
    bool modelError = false;
    // Module Branch
    auto outputs = net->onForward(inputs);
    for (int i=0; i<outputNames.size(); ++i) {
        auto name = outputNames[i];
        auto v = outputs[i];
        auto info = v->getInfo();
        if (nullptr == info) {
            continue;
        }
        if (info->order == MNN::Express::NC4HW4 && info->dim.size() > 1) {
            v = MNN::Express::_Convert(v, mInfo->defaultFormat);
        }
        if (info->type.code != halide_type_float) {
            v = MNN::Express::_Cast<float>(v);
        }
        v.fix(MNN::Express::VARP::CONSTANT);
        outputs[i] = v;
    }

    for (int i=0; i<outputNames.size(); ++i) {
        auto output = outputs[i];
        bool success = compareOutput(output, directName, outputNames[i], mInfo->defaultFormat, i, maxErrorRate);
        if (!success) {
            modelError = true;
            MNN_ERROR("Error for output %s\n", outputNames[i].c_str());
        }
    }

    if (modelError) {
        MNN_ERROR("Save mnn result to  .error director\n");
        for (int i=0; i<outputNames.size(); ++i) {
            auto v = outputs[i];
            auto name = outputNames[i];
            auto info = v->getInfo();
            if (nullptr == info) {
                continue;
            }
            if (info->order == MNN::Express::NC4HW4 && info->dim.size() > 1) {
                v = MNN::Express::_Convert(v, mInfo->defaultFormat);
            }
            if (info->type.code != halide_type_float) {
                v = MNN::Express::_Cast<float>(v);
            }
            v.fix(MNN::Express::VARP::CONSTANT);
            info = v->getInfo();
            std::ofstream _output((".error/" + name + ".txt").c_str());
            auto ptr = v->readMap<float>();
            for (int v=0; v<info->size; ++v) {
                _output << ptr[v] << "\n";
            }
            v->setName(name);
            outputs.emplace_back(v);
        }
        MNN::Express::Variable::save(outputs, ".Error.mnn");
        return 0;
    }
    MNN_PRINT("TEST_SUCCESS\n");
    return 0;
}

bool Cli::mnn2json(const char* modelFile, const char* jsonFile, int flag) {
    std::ifstream inputFile(modelFile, std::ios::binary);
    inputFile.seekg(0, std::ios::end);
    auto size = inputFile.tellg();
    inputFile.seekg(0, std::ios::beg);

    char* buffer = new char[size];

    inputFile.read((char*)buffer, size);
    std::ofstream output(jsonFile);

    if (flag > 3) {
        MNN_PRINT("Dont't add convweight\n");
        auto netT = MNN::UnPackNet((void*)buffer);
        auto treatFunction = [&](MNN::OpT* opParam) {
            auto type = opParam->main.type;
            if (type == MNN::OpParameter::OpParameter_Convolution2D) {
                auto param = opParam->main.AsConvolution2D();
                param->weight.clear();
                param->bias.clear();
                if (param->symmetricQuan) {
                    param->symmetricQuan->weight.clear();
                }
                if (param->quanParameter) {
                    param->quanParameter->buffer.clear();
                }
            } else if (type == MNN::OpParameter::OpParameter_Blob) {
                size_t totalSize = 1;
                auto blobT = opParam->main.AsBlob();
                for (int v=0; v<blobT->dims.size(); ++v) {
                    totalSize *= blobT->dims[v];
                }
                if (totalSize > 20) {
                    blobT->float32s.clear();
                    blobT->int8s.clear();
                    blobT->uint8s.clear();
                    blobT->int32s.clear();
                    blobT->int64s.clear();
                }
            } else if (type == MNN::OpParameter::OpParameter_Convolution2D) {
                opParam->main.AsConvolution2D()->weight.clear();
                opParam->main.AsConvolution2D()->bias.clear();
            } else if (type == MNN::OpParameter::OpParameter_MatMul) {
                opParam->main.AsMatMul()->weight.clear();
                opParam->main.AsMatMul()->bias.clear();
            } else if (type == MNN::OpParameter::OpParameter_PRelu) {
                opParam->main.AsPRelu()->slope.clear();
            } else if (type == MNN::OpParameter::OpParameter_Extra) {
                auto extra = opParam->main.AsExtra();
                extra->info.clear();
            } else if(type == MNN::OpParameter::OpParameter_LSTM){
                auto param = opParam->main.AsLSTM();
                if (param->weightH) {
                    param->weightH->float32s.clear();
                }
                if (param->weightI) {
                    param->weightI->float32s.clear();
                }
                if (param->bias) {
                    param->bias->float32s.clear();
                }
            }
        };
        for (int i = 0; i < netT->oplists.size(); ++i) {
            treatFunction(netT->oplists[i].get());
        }
        for (int i = 0; i < netT->subgraphs.size(); ++i) {
            for (int j=0; j<netT->subgraphs[i]->nodes.size(); ++j) {
                treatFunction(netT->subgraphs[i]->nodes[j].get());
            }
        }
        if (flag > 4) {
            printf("Separate dump subgraph\n");
            for (int i=0; i<netT->subgraphs.size(); ++i) {
                auto& g = netT->subgraphs[i];
                flatbuffers::FlatBufferBuilder newBuilder(1024);
                auto root = MNN::SubGraphProto::Pack(newBuilder, g.get());
                newBuilder.Finish(root);
                auto content = newBuilder.GetBufferPointer();
                char subGraphNameStr[128];
                sprintf(subGraphNameStr, "%s_%d", jsonFile, i);
                printf("Dump subgraph %s to %s\n", g->name.c_str(), subGraphNameStr);
                std::ofstream tempOutput(subGraphNameStr);
                auto s       = flatbuffers::FlatBufferToString((const uint8_t*)content, MNN::SubGraphProtoTypeTable());
                tempOutput << s;
            }
            netT->subgraphs.clear();
        }
        flatbuffers::FlatBufferBuilder newBuilder(1024);
        auto root = MNN::Net::Pack(newBuilder, netT.get());
        MNN::FinishNetBuffer(newBuilder, root);
        {
            auto content = newBuilder.GetBufferPointer();
            auto s       = flatbuffers::FlatBufferToString((const uint8_t*)content, MNN::NetTypeTable());
            output << s;
        }
    } else {
        auto s = flatbuffers::FlatBufferToString((const uint8_t*)buffer, MNN::NetTypeTable());
        output << s;
    }

    delete[] buffer;
    return true;
}

bool Cli::json2mnn(const char* jsonFile, const char* modelFile) {
    rapidjson::Document document;
    {
        std::ifstream fileNames(jsonFile);
        std::ostringstream output;
        output << fileNames.rdbuf();
        auto outputStr = output.str();
        document.Parse(outputStr.c_str());
        if (document.HasParseError()) {
            MNN_ERROR("Invalid json\n");
            return 0;
        }
    }
    auto object = document.GetObject();
    flatbuffers::FlatBufferBuilder builder;
    builder.ForceDefaults(true);
    auto table = MNN::NetTypeTable();
    auto offset = Json2Flatbuffer::writeJsonToFlatbuffer(table, builder, object);
    builder.Finish(offset);
    std::ofstream outputOs(modelFile, std::ios::binary);
    outputOs.write((char*)builder.GetBufferPointer(), builder.GetSize());
    return true;
}

};
