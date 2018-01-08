/* Copyright (c) 2017 Hans-Kristian Arntzen
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "smaa.hpp"
#include "math.hpp"

using namespace std;

namespace Granite
{
void setup_smaa_postprocess(RenderGraph &graph, const string &input, const string &output)
{
	const bool masked_edge = true;
	graph.get_texture_resource(input).get_attachment_info().unorm_srgb_alias = true;

	AttachmentInfo smaa_edge_output;
	smaa_edge_output.size_class = SizeClass::InputRelative;
	smaa_edge_output.size_relative_name = input;
	smaa_edge_output.format = VK_FORMAT_R8G8_UNORM;

	AttachmentInfo smaa_weight_output;
	smaa_weight_output.size_class = SizeClass::InputRelative;
	smaa_weight_output.size_relative_name = input;
	smaa_weight_output.format = VK_FORMAT_R8G8B8A8_UNORM;

	AttachmentInfo smaa_output;
	smaa_output.size_class = SizeClass::InputRelative;
	smaa_output.size_relative_name = input;

	AttachmentInfo smaa_depth;
	smaa_depth.size_class = SizeClass::InputRelative;
	smaa_depth.size_relative_name = input;
	smaa_depth.format = VK_FORMAT_D16_UNORM;

	auto &smaa_edge = graph.add_pass("smaa-edge", VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT);
	auto &smaa_weight = graph.add_pass("smaa-weights", VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT);
	auto &smaa_blend = graph.add_pass("smaa-blend", VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT);

	smaa_edge.add_color_output("smaa-edge", smaa_edge_output);
	smaa_edge.add_texture_input(input);
	if (masked_edge)
	{
		smaa_edge.set_depth_stencil_output("smaa-mask", smaa_depth);
		smaa_edge.set_get_clear_depth_stencil([&](VkClearDepthStencilValue *value) {
			if (value)
			{
				value->depth = 1.0f;
				value->stencil = 0;
			}
			return true;
		});
	}

	smaa_weight.add_color_output("smaa-weights", smaa_weight_output);
	smaa_weight.add_texture_input("smaa-edge");

	if (masked_edge)
		smaa_weight.set_depth_stencil_input("smaa-mask");

	smaa_blend.add_color_output(output, smaa_output);
	smaa_blend.add_texture_input(input);
	smaa_blend.add_texture_input("smaa-weights");

	smaa_edge.set_build_render_pass([&, masked_edge](Vulkan::CommandBuffer &cmd) {
		auto &input_image = graph.get_physical_texture_resource(smaa_edge.get_texture_inputs()[0]->get_physical_index());
		cmd.set_unorm_texture(0, 0, input_image);
		cmd.set_sampler(0, 0, Vulkan::StockSampler::LinearClamp);
		vec4 rt_metrics(1.0f / input_image.get_image().get_create_info().width,
		                1.0f / input_image.get_image().get_create_info().height,
		                float(input_image.get_image().get_create_info().width),
		                float(input_image.get_image().get_create_info().height));
		cmd.push_constants(&rt_metrics, 0, sizeof(rt_metrics));

		Vulkan::CommandBufferUtil::set_quad_vertex_state(cmd);
		Vulkan::CommandBufferUtil::draw_quad_depth(cmd,
		                                           "builtin://shaders/post/smaa_edge_detection.vert",
		                                           "builtin://shaders/post/smaa_edge_detection.frag",
		                                           masked_edge, masked_edge, VK_COMPARE_OP_ALWAYS, {});
	});

	smaa_edge.set_get_clear_color([](unsigned, VkClearColorValue *value) {
		if (value)
			memset(value, 0, sizeof(*value));
		return true;
	});

	smaa_weight.set_build_render_pass([&, masked_edge](Vulkan::CommandBuffer &cmd) {
		auto &input_image = graph.get_physical_texture_resource(smaa_weight.get_texture_inputs()[0]->get_physical_index());
		cmd.set_texture(0, 0, input_image, Vulkan::StockSampler::LinearClamp);
		cmd.set_texture(0, 1,
		                cmd.get_device().get_texture_manager().request_texture("builtin://textures/smaa/area.ktx")->get_image()->get_view(),
		                Vulkan::StockSampler::LinearClamp);
		cmd.set_texture(0, 2,
		                cmd.get_device().get_texture_manager().request_texture("builtin://textures/smaa/search.ktx")->get_image()->get_view(),
		                Vulkan::StockSampler::LinearClamp);
		vec4 rt_metrics(1.0f / input_image.get_image().get_create_info().width,
		                1.0f / input_image.get_image().get_create_info().height,
		                float(input_image.get_image().get_create_info().width),
		                float(input_image.get_image().get_create_info().height));
		cmd.push_constants(&rt_metrics, 0, sizeof(rt_metrics));

		Vulkan::CommandBufferUtil::set_quad_vertex_state(cmd);
		Vulkan::CommandBufferUtil::draw_quad_depth(cmd,
		                                           "builtin://shaders/post/smaa_blend_weight.vert",
		                                           "builtin://shaders/post/smaa_blend_weight.frag",
		                                           masked_edge, false, VK_COMPARE_OP_EQUAL, {});
	});

	smaa_weight.set_get_clear_color([](unsigned, VkClearColorValue *value) {
		if (value)
			memset(value, 0, sizeof(*value));
		return true;
	});

	smaa_blend.set_build_render_pass([&](Vulkan::CommandBuffer &cmd) {
		auto &input_image = graph.get_physical_texture_resource(smaa_blend.get_texture_inputs()[0]->get_physical_index());
		auto &blend_image = graph.get_physical_texture_resource(smaa_blend.get_texture_inputs()[1]->get_physical_index());
		cmd.set_texture(0, 0, input_image, Vulkan::StockSampler::LinearClamp);
		cmd.set_texture(0, 1, blend_image, Vulkan::StockSampler::LinearClamp);

		vec4 rt_metrics(1.0f / input_image.get_image().get_create_info().width,
		                1.0f / input_image.get_image().get_create_info().height,
		                float(input_image.get_image().get_create_info().width),
		                float(input_image.get_image().get_create_info().height));
		cmd.push_constants(&rt_metrics, 0, sizeof(rt_metrics));

		Vulkan::CommandBufferUtil::set_quad_vertex_state(cmd);
		Vulkan::CommandBufferUtil::draw_quad(cmd,
		                                     "builtin://shaders/post/smaa_neighbor_blend.vert",
		                                     "builtin://shaders/post/smaa_neighbor_blend.frag", {});
	});
}
}