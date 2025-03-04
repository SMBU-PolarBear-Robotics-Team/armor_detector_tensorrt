#include "tensorrt_armor_detector/TRTModule.hpp"

#include <NvOnnxParser.h>
#include <cuda_runtime_api.h>
#include <rclcpp/logging.hpp>

#include <fstream>

// #include <logger.h>
#define TRT_ASSERT(expr)                                                \
  do {                                                                  \
    if (!(expr)) {                                                      \
      fmt::print(fmt::fg(fmt::color::red), "assert fail: '" #expr "'"); \
      exit(-1);                                                         \
    }                                                                   \
  } while (0)

namespace rm_auto_aim
{
using namespace nvinfer1;
static const int INPUT_W = 416;        // Width of input
static const int INPUT_H = 416;        // Height of input
static constexpr int NUM_CLASSES = 8;  // Number of classes
static constexpr int NUM_COLORS = 4;   // Number of color
static constexpr float MERGE_CONF_ERROR = 0.15;
static constexpr float MERGE_MIN_IOU = 0.9;
// 辅助函数：生成网格和步长
struct GridAndStride
{
  int grid0;
  int grid1;
  int stride;
};
static void generate_grids_and_stride(
  std::vector<int> & strides, std::vector<GridAndStride> & grid_strides)
{
  for (auto stride : strides) {
    int num_grid_w = 416 / stride;
    int num_grid_h = 416 / stride;
    for (int g1 = 0; g1 < num_grid_h; g1++) {
      for (int g0 = 0; g0 < num_grid_w; g0++) {
        grid_strides.push_back(GridAndStride{g0, g1, stride});
      }
    }
  }
}
static cv::Mat letterbox(
  const cv::Mat & img, Eigen::Matrix3f & transform_matrix,
  std::vector<int> new_shape = {INPUT_W, INPUT_H})
{
  // Get current image shape [height, width]

  int img_h = img.rows;
  int img_w = img.cols;

  // Compute scale ratio(new / old) and target resized shape
  float scale = std::min(new_shape[1] * 1.0 / img_h, new_shape[0] * 1.0 / img_w);
  int resize_h = static_cast<int>(round(img_h * scale));
  int resize_w = static_cast<int>(round(img_w * scale));

  // Compute padding
  int pad_h = new_shape[1] - resize_h;
  int pad_w = new_shape[0] - resize_w;

  // Resize and pad image while meeting stride-multiple constraints
  cv::Mat resized_img;
  cv::resize(img, resized_img, cv::Size(resize_w, resize_h));

  // divide padding into 2 sides
  float half_h = pad_h * 1.0 / 2;
  float half_w = pad_w * 1.0 / 2;

  // Compute padding boarder
  int top = static_cast<int>(round(half_h - 0.1));
  int bottom = static_cast<int>(round(half_h + 0.1));
  int left = static_cast<int>(round(half_w - 0.1));
  int right = static_cast<int>(round(half_w + 0.1));

  /* clang-format off */
  /* *INDENT-OFF* */

  // Compute point transform_matrix
  transform_matrix << 1.0 / scale, 0, -half_w / scale,
                      0, 1.0 / scale, -half_h / scale,
                      0, 0, 1;

  /* *INDENT-ON* */
  /* clang-format on */

  // Add border
  cv::copyMakeBorder(
    resized_img, resized_img, top, bottom, left, right, cv::BORDER_CONSTANT,
    cv::Scalar(114, 114, 114));

  return resized_img;
}
/**
 * @brief Calculate intersection area between two objects.
 * @param a Object a.
 * @param b Object b.
 * @return Area of intersection.
 */
static inline float intersection_area(const ArmorObject & a, const ArmorObject & b)
{
  cv::Rect_<float> inter = a.box & b.box;
  return inter.area();
}

static void nms_merge_sorted_bboxes(
  std::vector<ArmorObject> & faceobjects, std::vector<int> & indices, float nms_threshold)
{
  indices.clear();

  const int n = faceobjects.size();

  std::vector<float> areas(n);
  for (int i = 0; i < n; i++) {
    areas[i] = faceobjects[i].box.area();
  }

  for (int i = 0; i < n; i++) {
    ArmorObject & a = faceobjects[i];

    int keep = 1;
    for (size_t j = 0; j < indices.size(); j++) {
      ArmorObject & b = faceobjects[indices[j]];

      // intersection over union
      float inter_area = intersection_area(a, b);
      float union_area = areas[i] + areas[indices[j]] - inter_area;
      float iou = inter_area / union_area;
      if (iou > nms_threshold || isnan(iou)) {
        keep = 0;
        // Stored for Merge
        if (
          a.number == b.number && a.color == b.color && iou > MERGE_MIN_IOU &&
          abs(a.prob - b.prob) < MERGE_CONF_ERROR) {
          for (int i = 0; i < 4; i++) {
            b.pts.push_back(a.pts[i]);
          }
        }
        // cout<<b.pts_x.size()<<endl;
      }
    }

    if (keep) {
      indices.push_back(i);
    }
  }
}

// 构造函数：初始化参数并构建引擎
AdaptedTRTModule::AdaptedTRTModule(const std::string & onnx_path, const Params & params)
: params_(params), engine_(nullptr), context_(nullptr), output_buffer_(nullptr)
{
  build_engine(onnx_path);
  TRT_ASSERT(context_ = engine_->createExecutionContext());
  TRT_ASSERT((input_idx_ = engine_->getBindingIndex("images")) == 0);
  TRT_ASSERT((output_idx_ = engine_->getBindingIndex("output")) == 1);

  // 分配显存
  auto input_dims = engine_->getBindingDimensions(input_idx_);
  auto output_dims = engine_->getBindingDimensions(output_idx_);
  input_sz_ = input_dims.d[1] * input_dims.d[2] * input_dims.d[3];  // 1x3x416x416
  output_sz_ = output_dims.d[1] * output_dims.d[2];                 // 1x21x3549
  // RCLCPP_INFO(rclcpp::get_logger("postprocess.output_buff"), "output_sz: %ld, output_dims.d[1]: %d, output_dims.d[2]: %d", output_sz_, output_dims.d[1], output_dims.d[2]);
  TRT_ASSERT(cudaMalloc(&device_buffers_[input_idx_], input_sz_ * sizeof(float)) == 0);
  TRT_ASSERT(cudaMalloc(&device_buffers_[output_idx_], output_sz_ * sizeof(float)) == 0);
  output_buffer_ = new float[output_sz_];
  TRT_ASSERT(cudaStreamCreate(&stream_) == 0);
}

// 析构函数：释放资源
AdaptedTRTModule::~AdaptedTRTModule()
{
  delete[] output_buffer_;
  cudaStreamDestroy(stream_);
  cudaFree(device_buffers_[output_idx_]);
  cudaFree(device_buffers_[input_idx_]);
  if (engine_) engine_->destroy();
  if (context_) context_->destroy();
}

// 构建 TensorRT 引擎
void AdaptedTRTModule::build_engine(const std::string & onnx_path)
{
  // 生成引擎文件路径（与ONNX同目录，后缀改为.engine）
  std::string engine_path = onnx_path.substr(0, onnx_path.find_last_of('.')) + ".engine";
  
  // 尝试加载缓存的引擎文件
  std::ifstream engine_file(engine_path, std::ios::binary);
  if (engine_file.good()) {
    engine_file.seekg(0, std::ios::end);
    size_t size = engine_file.tellg();
    engine_file.seekg(0, std::ios::beg);
    std::vector<char> engine_data(size);
    engine_file.read(engine_data.data(), size);
    engine_file.close();

    auto runtime = nvinfer1::createInferRuntime(gLogger);
    engine_ = runtime->deserializeCudaEngine(engine_data.data(), size);
    runtime->destroy();
    
    if (engine_ != nullptr) {
      RCLCPP_INFO(rclcpp::get_logger("TRTModule"), "Loaded cached engine: %s", engine_path.c_str());
      return;
    }
  }

  // 构建新引擎
  auto builder = nvinfer1::createInferBuilder(gLogger);
  const auto explicit_batch = 1U << static_cast<uint32_t>(nvinfer1::NetworkDefinitionCreationFlag::kEXPLICIT_BATCH);
  auto network = builder->createNetworkV2(explicit_batch);
  auto parser = nvonnxparser::createParser(*network, gLogger);
  parser->parseFromFile(onnx_path.c_str(), static_cast<int>(nvinfer1::ILogger::Severity::kINFO));

  auto config = builder->createBuilderConfig();
  if (builder->platformHasFastFp16()) {
    config->setFlag(nvinfer1::BuilderFlag::kFP16);
  }
  engine_ = builder->buildEngineWithConfig(*network, *config);

  // 保存引擎到文件
  auto serialized_engine = engine_->serialize();
  std::ofstream out_file(engine_path, std::ios::binary);
  out_file.write(reinterpret_cast<const char*>(serialized_engine->data()), serialized_engine->size());
  out_file.close();
  serialized_engine->destroy();

  // 清理资源
  parser->destroy();
  network->destroy();
  config->destroy();
  builder->destroy();

  RCLCPP_INFO(rclcpp::get_logger("TRTModule"), "Saved new engine to: %s", engine_path.c_str());
}

// 推理函数
std::vector<ArmorObject> AdaptedTRTModule::detect(const cv::Mat & image)
{
  // 预处理：Letterbox 缩放
  // cv::Mat resized;
  // cv::resize(image, resized, cv::Size(params_.input_w, params_.input_h));

  Eigen::Matrix3f transform_matrix;  // transform matrix from resized image to source image.
  cv::Mat resized = letterbox(image, transform_matrix);

  // cv::Mat blob =
  //   cv::dnn::blobFromImage(resized, 1.0 / 255.0, cv::Size(), cv::Scalar(0, 0, 0), true);
  cv::Mat blob =
    cv::dnn::blobFromImage(resized, 1., cv::Size(INPUT_W, INPUT_H), cv::Scalar(0, 0, 0), true);
  // 拷贝数据到显存
  cudaMemcpyAsync(
    device_buffers_[input_idx_], blob.ptr<float>(), input_sz_ * sizeof(float),
    cudaMemcpyHostToDevice, stream_);
  context_->enqueueV2(device_buffers_, stream_, nullptr);
  cudaMemcpyAsync(
    output_buffer_, device_buffers_[output_idx_], output_sz_ * sizeof(float),
    cudaMemcpyDeviceToHost, stream_);
  cudaStreamSynchronize(stream_);

  std::vector<ArmorObject> objs_tmp, objs_result;
  std::vector<cv::Rect> rects;
  std::vector<float> scores;
  // 后处理
  return postprocess(objs_tmp, scores, rects, output_buffer_, output_sz_ / 21, transform_matrix);  // 3549个检测框
}

// 后处理函数
std::vector<ArmorObject> AdaptedTRTModule::postprocess(std::vector<ArmorObject> & output_objs, std::vector<float> & scores, std::vector<cv::Rect> & rects, 
const float * output, int num_detections, const Eigen::Matrix<float, 3, 3> & transform_matrix)
{
  std::vector<int> strides = {8, 16, 32};
  std::vector<GridAndStride> grid_strides;
  generate_grids_and_stride(strides, grid_strides);
  // RCLCPP_INFO(rclcpp::get_logger("postprocess.num_detections"), "num_detections: %d ", num_detections);


  for (int i = 0; i < num_detections; ++i) {
    const float * det = output + i * 21;
    float conf = det[8];
    // for (int j = 0; j < 21; ++j) {
    //   RCLCPP_INFO(rclcpp::get_logger("postprocess.conf"), "det[%d]: %f ", j, det[j]);
    // }
    if (conf < params_.conf_threshold) continue;

    // 解析坐标
    int grid0 = grid_strides[i].grid0;
    int grid1 = grid_strides[i].grid1;
    int stride = grid_strides[i].stride;
    // // 第一个点（左上）
    // box.pts[0].x = (det[0] + grid0) * stride;
    // box.pts[0].y = (det[1] + grid1) * stride;

    // // 第二个点（右上）
    // box.pts[1].x = (det[2] + grid0) * stride;
    // box.pts[1].y = (det[3] + grid1) * stride;

    // // 第三个点（右下）
    // box.pts[2].x = (det[4] + grid0) * stride;
    // box.pts[2].y = (det[5] + grid1) * stride;

    // // 第四个点（左下）
    // box.pts[3].x = (det[6] + grid0) * stride;
    // box.pts[3].y = (det[7] + grid1) * stride;
    cv::Point color_id, num_id;

    float x_1 = (det[0] + grid0) * stride;
    float y_1 = (det[1] + grid1) * stride;
    float x_2 = (det[2] + grid0) * stride;
    float y_2 = (det[3] + grid1) * stride;
    float x_3 = (det[4] + grid0) * stride;
    float y_3 = (det[5] + grid1) * stride;
    float x_4 = (det[6] + grid0) * stride;
    float y_4 = (det[7] + grid1) * stride;

    Eigen::Matrix<float, 3, 4> apex_norm;
    Eigen::Matrix<float, 3, 4> apex_dst;

    apex_norm << x_1, x_2, x_3, x_4,
                y_1, y_2, y_3, y_4,
                1,   1,   1,   1;

    apex_dst = transform_matrix * apex_norm;

    ArmorObject obj;

    obj.pts.resize(4);

    obj.pts[0] = cv::Point2f(apex_dst(0, 0), apex_dst(1, 0));
    obj.pts[1] = cv::Point2f(apex_dst(0, 1), apex_dst(1, 1));
    obj.pts[2] = cv::Point2f(apex_dst(0, 2), apex_dst(1, 2));
    obj.pts[3] = cv::Point2f(apex_dst(0, 3), apex_dst(1, 3));
    // RCLCPP_INFO(rclcpp::get_logger("postprocess.pos"), "obj_conf: %f, x_1: %f, y_1: %f, x_2: %f, y_2: %f, x_3: %f, y_3: %f, x_4: %f, y_4: %f",
    // conf, obj.pts[0].x, obj.pts[0].y, 
    // obj.pts[1].x, obj.pts[1].y, 
    // obj.pts[2].x, obj.pts[2].y, 
    // obj.pts[3].x, obj.pts[3].y);

    auto rect = cv::boundingRect(obj.pts);

    obj.box = rect;
    // obj.color = static_cast<ArmorColor>(color_id.x);
    // obj.number = static_cast<ArmorNumber>(num_id.x);
    obj.prob = conf;

    // 解析颜色和类别
    obj.color = static_cast<ArmorColor>(std::max_element(det + 9, det + 13) - (det + 9));
    obj.number = static_cast<ArmorNumber>(std::max_element(det + 13, det + 21) - (det + 13));
    // box.confidence = conf;

    rects.push_back(rect);
    scores.push_back(conf);
    output_objs.push_back(std::move(obj));
  }
  // RCLCPP_INFO(rclcpp::get_logger("postprocess.bboxs_size"), "Detected %d objects", bboxes.size());
  // NMS处理
  // 生成 cv::Rect 向量
  // std::vector<cv::Rect> rects;
  // for (const auto & bbox : bboxes) {
  //   int x = bbox.pts[0].x;
  //   int y = bbox.pts[0].y;
  //   int width = bbox.pts[2].x - bbox.pts[0].x;
  //   int height = bbox.pts[2].y - bbox.pts[0].y;
  //   rects.emplace_back(x, y, width, height);
  // }

  // // 生成 scores 向量
  // std::vector<float> scores;
  // for (const auto & bbox : bboxes) {
  //   scores.push_back(bbox.confidence);
  // }
 // TopK
  std::sort(output_objs.begin(), output_objs.end(), [](const ArmorObject & a, const ArmorObject & b) {
    return a.prob > b.prob;
  });
  if (output_objs.size() > static_cast<size_t>(params_.top_k)) {
    output_objs.resize(params_.top_k);
  }
  std::vector<int> indices;
  std::vector<ArmorObject> objs_result;
  // cv::dnn::NMSBoxes(rects, scores, params_.conf_threshold, params_.nms_threshold, indices);
  nms_merge_sorted_bboxes(output_objs, indices, params_.nms_threshold);

  for (size_t i = 0; i < indices.size(); i++) {
    objs_result.push_back(std::move(output_objs[indices[i]]));

    if (objs_result[i].pts.size() >= 8) {
      auto n = objs_result[i].pts.size();
      cv::Point2f pts_final[4];

      for (size_t j = 0; j < n; j++) {
        pts_final[j % 4] += objs_result[i].pts[j];
      }

      objs_result[i].pts.resize(4);
      for (int j = 0; j < 4; j++) {
        pts_final[j].x /= static_cast<float>(n) / 4.0;
        pts_final[j].y /= static_cast<float>(n) / 4.0;
        objs_result[i].pts[j] = pts_final[j];
      }
    }
  }

  return objs_result;
}
}  // namespace rm_auto_aim
