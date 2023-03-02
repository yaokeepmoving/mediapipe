// Copyright 2023 The MediaPipe Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <memory>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "mediapipe/framework/api2/node.h"
#include "mediapipe/framework/api2/packet.h"
#include "mediapipe/framework/api2/port.h"
#include "mediapipe/framework/calculator_framework.h"
#include "mediapipe/framework/calculator_options.pb.h"
#include "mediapipe/framework/formats/image.h"
#include "mediapipe/framework/formats/tensor.h"
#include "mediapipe/framework/port/logging.h"
#include "mediapipe/framework/port/status.h"
#include "mediapipe/gpu/gpu_origin.pb.h"
#include "mediapipe/tasks/cc/vision/face_stylizer/calculators/tensors_to_image_calculator.pb.h"

#if !MEDIAPIPE_DISABLE_GPU
#include "mediapipe/gpu/gpu_buffer.h"
#if MEDIAPIPE_METAL_ENABLED
#import <CoreVideo/CoreVideo.h>
#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>

#include "mediapipe/framework/formats/tensor_mtl_buffer_view.h"
#import "mediapipe/gpu/MPPMetalHelper.h"
#else
#include "mediapipe/gpu/gl_calculator_helper.h"
#include "mediapipe/gpu/gl_quad_renderer.h"
#include "mediapipe/gpu/gl_simple_shaders.h"
#if MEDIAPIPE_OPENGL_ES_VERSION >= MEDIAPIPE_OPENGL_ES_31
#include "tensorflow/lite/delegates/gpu/common/util.h"
#include "tensorflow/lite/delegates/gpu/gl/converters/util.h"
#include "tensorflow/lite/delegates/gpu/gl/gl_program.h"
#include "tensorflow/lite/delegates/gpu/gl/gl_shader.h"
#include "tensorflow/lite/delegates/gpu/gl/gl_texture.h"
#include "tensorflow/lite/delegates/gpu/gl_delegate.h"
#endif  // MEDIAPIPE_OPENGL_ES_VERSION >= MEDIAPIPE_OPENGL_ES_31
#endif  // MEDIAPIPE_METAL_ENABLED
#endif  // !MEDIAPIPE_DISABLE_GPU

namespace mediapipe {
namespace tasks {
namespace {

using ::mediapipe::api2::Input;
using ::mediapipe::api2::Node;
using ::mediapipe::api2::Output;

#if MEDIAPIPE_OPENGL_ES_VERSION >= MEDIAPIPE_OPENGL_ES_31
using ::tflite::gpu::gl::GlProgram;
using ::tflite::gpu::gl::GlShader;
#endif  // MEDIAPIPE_OPENGL_ES_VERSION >= MEDIAPIPE_OPENGL_ES_31

enum { ATTRIB_VERTEX, ATTRIB_TEXTURE_POSITION, NUM_ATTRIBUTES };

// Commonly used to compute the number of blocks to launch in a kernel.
static int NumGroups(const int size, const int group_size) {  // NOLINT
  return (size + group_size - 1) / group_size;
}

}  // namespace

// Converts a MediaPipe tensor to a MediaPipe Image.
//
// Input streams:
//   TENSORS - std::vector<mediapipe::Tensor> that only contains one element.
//
// Output streams:
//   OUTPUT - mediapipe::Image.
//
// TODO: Enable TensorsToImageCalculator to run on CPU.
class TensorsToImageCalculator : public Node {
 public:
  static constexpr Input<std::vector<Tensor>> kInputTensors{"TENSORS"};
  static constexpr Output<Image> kOutputImage{"IMAGE"};

  MEDIAPIPE_NODE_CONTRACT(kInputTensors, kOutputImage);

  static absl::Status UpdateContract(CalculatorContract* cc);
  absl::Status Open(CalculatorContext* cc);
  absl::Status Process(CalculatorContext* cc);
  absl::Status Close(CalculatorContext* cc);

 private:
#if !MEDIAPIPE_DISABLE_GPU
#if MEDIAPIPE_METAL_ENABLED
  bool metal_initialized_ = false;
  MPPMetalHelper* gpu_helper_ = nullptr;
  id<MTLComputePipelineState> to_buffer_program_;

  absl::Status MetalSetup(CalculatorContext* cc);
  absl::Status MetalProcess(CalculatorContext* cc);
#else
  absl::Status GlSetup(CalculatorContext* cc);

  GlCalculatorHelper gl_helper_;

  bool gl_initialized_ = false;
#if MEDIAPIPE_OPENGL_ES_VERSION >= MEDIAPIPE_OPENGL_ES_31
  std::unique_ptr<tflite::gpu::gl::GlProgram> gl_compute_program_;
  const tflite::gpu::uint3 workgroup_size_ = {8, 8, 1};
#else
  GLuint program_ = 0;
  std::unique_ptr<mediapipe::QuadRenderer> gl_renderer_;
#endif  // MEDIAPIPE_OPENGL_ES_VERSION >= MEDIAPIPE_OPENGL_ES_31
#endif  // MEDIAPIPE_METAL_ENABLED
#endif  // !MEDIAPIPE_DISABLE_GPU
};
MEDIAPIPE_REGISTER_NODE(::mediapipe::tasks::TensorsToImageCalculator);

absl::Status TensorsToImageCalculator::UpdateContract(CalculatorContract* cc) {
#if !MEDIAPIPE_DISABLE_GPU
#if MEDIAPIPE_METAL_ENABLED
  MP_RETURN_IF_ERROR([MPPMetalHelper updateContract:cc]);
#else
  return GlCalculatorHelper::UpdateContract(cc);
#endif  // MEDIAPIPE_METAL_ENABLED
#endif  // !MEDIAPIPE_DISABLE_GPU
  return absl::OkStatus();
}

absl::Status TensorsToImageCalculator::Open(CalculatorContext* cc) {
#if !MEDIAPIPE_DISABLE_GPU
#if MEDIAPIPE_METAL_ENABLED
  gpu_helper_ = [[MPPMetalHelper alloc] initWithCalculatorContext:cc];
  RET_CHECK(gpu_helper_);
#else
  MP_RETURN_IF_ERROR(gl_helper_.Open(cc));
#endif  // MEDIAPIPE_METAL_ENABLED
#endif  // !MEDIAPIPE_DISABLE_GPU

  return absl::OkStatus();
}

absl::Status TensorsToImageCalculator::Process(CalculatorContext* cc) {
#if !MEDIAPIPE_DISABLE_GPU
#if MEDIAPIPE_METAL_ENABLED

  return MetalProcess(cc);

#else

  return gl_helper_.RunInGlContext([this, cc]() -> absl::Status {
    if (!gl_initialized_) {
      MP_RETURN_IF_ERROR(GlSetup(cc));
      gl_initialized_ = true;
    }

    if (kInputTensors(cc).IsEmpty()) {
      return absl::OkStatus();
    }
    const auto& input_tensors = kInputTensors(cc).Get();
    RET_CHECK_EQ(input_tensors.size(), 1)
        << "Expect 1 input tensor, but have " << input_tensors.size();
    const int tensor_width = input_tensors[0].shape().dims[2];
    const int tensor_height = input_tensors[0].shape().dims[1];

#if MEDIAPIPE_OPENGL_ES_VERSION >= MEDIAPIPE_OPENGL_ES_31

    auto out_texture = std::make_unique<tflite::gpu::gl::GlTexture>();
    MP_RETURN_IF_ERROR(CreateReadWriteRgbaImageTexture(
        tflite::gpu::DataType::UINT8,  // GL_RGBA8
        {tensor_width, tensor_height}, out_texture.get()));

    const int output_index = 0;
    glBindImageTexture(output_index, out_texture->id(), 0, GL_FALSE, 0,
                       GL_WRITE_ONLY, GL_RGBA8);

    auto read_view = input_tensors[0].GetOpenGlBufferReadView();
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, read_view.name());

    const tflite::gpu::uint3 workload = {tensor_width, tensor_height, 1};
    const tflite::gpu::uint3 workgroups =
        tflite::gpu::DivideRoundUp(workload, workgroup_size_);

    glUseProgram(gl_compute_program_->id());
    glUniform2i(glGetUniformLocation(gl_compute_program_->id(), "out_size"),
                tensor_width, tensor_height);

    MP_RETURN_IF_ERROR(gl_compute_program_->Dispatch(workgroups));

    auto texture_buffer = mediapipe::GlTextureBuffer::Wrap(
        out_texture->target(), out_texture->id(), tensor_width, tensor_height,
        mediapipe::GpuBufferFormat::kBGRA32,
        [ptr = out_texture.release()](
            std::shared_ptr<mediapipe::GlSyncPoint> sync_token) mutable {
          delete ptr;
        });

    auto output =
        std::make_unique<mediapipe::GpuBuffer>(std::move(texture_buffer));
    kOutputImage(cc).Send(Image(*output));
    ;

#else

    if (!input_tensors[0].ready_as_opengl_texture_2d()) {
      (void)input_tensors[0].GetCpuReadView();
    }

    auto output_texture =
        gl_helper_.CreateDestinationTexture(tensor_width, tensor_height);
    gl_helper_.BindFramebuffer(output_texture);  // GL_TEXTURE0
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D,
                  input_tensors[0].GetOpenGlTexture2dReadView().name());

    MP_RETURN_IF_ERROR(gl_renderer_->GlRender(
        tensor_width, tensor_height, output_texture.width(),
        output_texture.height(), mediapipe::FrameScaleMode::kStretch,
        mediapipe::FrameRotation::kNone,
        /*flip_horizontal=*/false, /*flip_vertical=*/false,
        /*flip_texture=*/false));

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, 0);

    auto output = output_texture.GetFrame<GpuBuffer>();
    kOutputImage(cc).Send(Image(*output));

#endif  // MEDIAPIPE_OPENGL_ES_VERSION >= MEDIAPIPE_OPENGL_ES_31

    return mediapipe::OkStatus();
  });

#endif  // MEDIAPIPE_METAL_ENABLED
#endif  // !MEDIAPIPE_DISABLE_GPU
  return absl::OkStatus();
}

absl::Status TensorsToImageCalculator::Close(CalculatorContext* cc) {
#if !MEDIAPIPE_DISABLE_GPU && !MEDIAPIPE_METAL_ENABLED
  gl_helper_.RunInGlContext([this] {
#if MEDIAPIPE_OPENGL_ES_VERSION >= MEDIAPIPE_OPENGL_ES_31
    gl_compute_program_.reset();
#else
    if (program_) glDeleteProgram(program_);
    program_ = 0;
#endif  // MEDIAPIPE_OPENGL_ES_VERSION >= MEDIAPIPE_OPENGL_ES_31
  });
#endif  // !MEDIAPIPE_DISABLE_GPU && !MEDIAPIPE_METAL_ENABLED
  return absl::OkStatus();
}

#if MEDIAPIPE_METAL_ENABLED

absl::Status TensorsToImageCalculator::MetalProcess(CalculatorContext* cc) {
  if (!metal_initialized_) {
    MP_RETURN_IF_ERROR(MetalSetup(cc));
    metal_initialized_ = true;
  }

  if (kInputTensors(cc).IsEmpty()) {
    return absl::OkStatus();
  }
  const auto& input_tensors = kInputTensors(cc).Get();
  RET_CHECK_EQ(input_tensors.size(), 1)
      << "Expect 1 input tensor, but have " << input_tensors.size();
  const int tensor_width = input_tensors[0].shape().dims[2];
  const int tensor_height = input_tensors[0].shape().dims[1];

  // TODO: Fix unused variable
  [[maybe_unused]] id<MTLDevice> device = gpu_helper_.mtlDevice;
  id<MTLCommandBuffer> command_buffer = [gpu_helper_ commandBuffer];
  command_buffer.label = @"TensorsToImageCalculatorConvert";
  id<MTLComputeCommandEncoder> compute_encoder =
      [command_buffer computeCommandEncoder];
  [compute_encoder setComputePipelineState:to_buffer_program_];

  auto input_view =
      mediapipe::MtlBufferView::GetReadView(input_tensors[0], command_buffer);
  [compute_encoder setBuffer:input_view.buffer() offset:0 atIndex:0];

  mediapipe::GpuBuffer output =
      [gpu_helper_ mediapipeGpuBufferWithWidth:tensor_width
                                        height:tensor_height];
  id<MTLTexture> dst_texture = [gpu_helper_ metalTextureWithGpuBuffer:output];
  [compute_encoder setTexture:dst_texture atIndex:1];

  MTLSize threads_per_group = MTLSizeMake(8, 8, 1);
  MTLSize threadgroups =
      MTLSizeMake(NumGroups(tensor_width, 8), NumGroups(tensor_height, 8), 1);
  [compute_encoder dispatchThreadgroups:threadgroups
                  threadsPerThreadgroup:threads_per_group];
  [compute_encoder endEncoding];
  [command_buffer commit];

  kOutputImage(cc).Send(Image(output));
  return absl::OkStatus();
}

absl::Status TensorsToImageCalculator::MetalSetup(CalculatorContext* cc) {
  id<MTLDevice> device = gpu_helper_.mtlDevice;
  const std::string shader_source =
      R"(
  #include <metal_stdlib>

  using namespace metal;

  kernel void convertKernel(
      device float*                         in_buf   [[ buffer(0) ]],
      texture2d<float, access::read_write>  out_tex  [[ texture(1) ]],
      uint2                                 gid      [[ thread_position_in_grid ]]) {
        if (gid.x >= out_tex.get_width() || gid.y >= out_tex.get_height()) return;
        uint linear_index = 3 * (gid.y * out_tex.get_width() + gid.x);
        float4 out_value = float4(in_buf[linear_index], in_buf[linear_index + 1], in_buf[linear_index + 2], 1.0);
        out_tex.write(out_value, gid);
      }
  )";
  NSString* library_source =
      [NSString stringWithUTF8String:shader_source.c_str()];
  NSError* error = nil;
  id<MTLLibrary> library =
      [device newLibraryWithSource:library_source options:nullptr error:&error];
  RET_CHECK(library != nil) << "Couldn't create shader library "
                            << [[error localizedDescription] UTF8String];
  id<MTLFunction> kernel_func = nil;
  kernel_func = [library newFunctionWithName:@"convertKernel"];
  RET_CHECK(kernel_func != nil) << "Couldn't create kernel function.";
  to_buffer_program_ =
      [device newComputePipelineStateWithFunction:kernel_func error:&error];
  RET_CHECK(to_buffer_program_ != nil) << "Couldn't create pipeline state " <<
      [[error localizedDescription] UTF8String];

  return mediapipe::OkStatus();
}

#endif  // MEDIAPIPE_METAL_ENABLED

#if !MEDIAPIPE_DISABLE_GPU && !MEDIAPIPE_METAL_ENABLED
absl::Status TensorsToImageCalculator::GlSetup(CalculatorContext* cc) {
  std::string maybe_flip_y_define;
#if !defined(__APPLE__)
  const auto& options = cc->Options<TensorsToImageCalculatorOptions>();
  if (options.gpu_origin() != mediapipe::GpuOrigin::TOP_LEFT) {
    maybe_flip_y_define = R"(
      #define FLIP_Y_COORD
    )";
  }
#endif  // !defined(__APPLE__)

#if MEDIAPIPE_OPENGL_ES_VERSION >= MEDIAPIPE_OPENGL_ES_31

  const std::string shader_header =
      absl::StrCat(tflite::gpu::gl::GetShaderHeader(workgroup_size_), R"(
    precision highp float;
    layout(rgba8, binding = 0) writeonly uniform highp image2D output_texture;
    uniform ivec2 out_size;
  )");

  const std::string shader_body = R"(
    layout(std430, binding = 2) readonly buffer B0 {
      float elements[];
    } input_data;   // data tensor

    void main() {
      int out_width = out_size.x;
      int out_height = out_size.y;

      ivec2 gid = ivec2(gl_GlobalInvocationID.xy);
      if (gid.x >= out_width || gid.y >= out_height) { return; }
      int linear_index = 3 * (gid.y * out_width + gid.x);

#ifdef FLIP_Y_COORD
      int y_coord = out_height - gid.y - 1;
#else
      int y_coord = gid.y;
#endif  // defined(FLIP_Y_COORD)

      ivec2 out_coordinate = ivec2(gid.x, y_coord);
      vec4 out_value = vec4(input_data.elements[linear_index], input_data.elements[linear_index + 1], input_data.elements[linear_index + 2], 1.0);
      imageStore(output_texture, out_coordinate, out_value);
    })";

  const std::string shader_full =
      absl::StrCat(shader_header, maybe_flip_y_define, shader_body);

  GlShader shader;
  MP_RETURN_IF_ERROR(
      GlShader::CompileShader(GL_COMPUTE_SHADER, shader_full, &shader));
  gl_compute_program_ = std::make_unique<GlProgram>();
  MP_RETURN_IF_ERROR(
      GlProgram::CreateWithShader(shader, gl_compute_program_.get()));

#else
  constexpr GLchar kFragColorOutputDeclaration[] = R"(
  #ifdef GL_ES
    #define fragColor gl_FragColor
  #else
    out vec4 fragColor;
  #endif  // defined(GL_ES);
)";

  constexpr GLchar kBody[] = R"(
    DEFAULT_PRECISION(mediump, float)
    in vec2 sample_coordinate;
    uniform sampler2D tensor;
    void main() {
#ifdef FLIP_Y_COORD
      float y_coord = 1.0 - sample_coordinate.y;
#else
      float y_coord = sample_coordinate.y;
#endif  // defined(FLIP_Y_COORD)
      vec3 color = texture2D(tensor, vec2(sample_coordinate.x, y_coord)).rgb;
      fragColor = vec4(color, 1.0);
    }
  )";

  const std::string src =
      absl::StrCat(mediapipe::kMediaPipeFragmentShaderPreamble,
                   kFragColorOutputDeclaration, maybe_flip_y_define, kBody);
  gl_renderer_ = std::make_unique<mediapipe::QuadRenderer>();
  MP_RETURN_IF_ERROR(gl_renderer_->GlSetup(src.c_str(), {"tensor"}));

#endif  // MEDIAPIPE_OPENGL_ES_VERSION >= MEDIAPIPE_OPENGL_ES_31

  return mediapipe::OkStatus();
}

#endif  // !MEDIAPIPE_DISABLE_GPU && !MEDIAPIPE_METAL_ENABLED

}  // namespace tasks
}  // namespace mediapipe