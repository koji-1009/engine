// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/display_list/dl_builder.h"

#include "flutter/display_list/display_list.h"
#include "flutter/display_list/dl_blend_mode.h"
#include "flutter/display_list/dl_op_flags.h"
#include "flutter/display_list/dl_op_records.h"
#include "flutter/display_list/effects/dl_color_source.h"
#include "flutter/display_list/utils/dl_accumulation_rect.h"
#include "fml/logging.h"
#include "third_party/skia/include/core/SkScalar.h"

namespace flutter {

#define DL_BUILDER_PAGE 4096

// CopyV(dst, src,n, src,n, ...) copies any number of typed srcs into dst.
static void CopyV(void* dst) {}

template <typename S, typename... Rest>
static void CopyV(void* dst, const S* src, int n, Rest&&... rest) {
  FML_DCHECK(((uintptr_t)dst & (alignof(S) - 1)) == 0)
      << "Expected " << dst << " to be aligned for at least " << alignof(S)
      << " bytes.";
  // If n is 0, there is nothing to copy into dst from src.
  if (n > 0) {
    memcpy(dst, src, n * sizeof(S));
    dst = reinterpret_cast<void*>(reinterpret_cast<uint8_t*>(dst) +
                                  n * sizeof(S));
  }
  // Repeat for the next items, if any
  CopyV(dst, std::forward<Rest>(rest)...);
}

static constexpr inline bool is_power_of_two(int value) {
  return (value & (value - 1)) == 0;
}

template <typename T, typename... Args>
void* DisplayListBuilder::Push(size_t pod, Args&&... args) {
  size_t size = SkAlignPtr(sizeof(T) + pod);
  FML_CHECK(size < (1 << 24));
  if (used_ + size > allocated_) {
    static_assert(is_power_of_two(DL_BUILDER_PAGE),
                  "This math needs updating for non-pow2.");
    // Next greater multiple of DL_BUILDER_PAGE.
    allocated_ = (used_ + size + DL_BUILDER_PAGE) & ~(DL_BUILDER_PAGE - 1);
    storage_.realloc(allocated_);
    FML_CHECK(storage_.get());
    memset(storage_.get() + used_, 0, allocated_ - used_);
  }
  FML_CHECK(used_ + size <= allocated_);
  auto op = reinterpret_cast<T*>(storage_.get() + used_);
  used_ += size;
  new (op) T{std::forward<Args>(args)...};
  op->type = T::kType;
  op->size = size;
  render_op_count_ += T::kRenderOpInc;
  depth_ += T::kDepthInc * render_op_depth_cost_;
  op_index_++;
  return op + 1;
}

sk_sp<DisplayList> DisplayListBuilder::Build() {
  while (save_stack_.size() > 1) {
    restore();
  }

  size_t bytes = used_;
  int count = render_op_count_;
  size_t nested_bytes = nested_bytes_;
  int nested_count = nested_op_count_;
  uint32_t total_depth = depth_;
  bool opacity_compatible = current_layer().is_group_opacity_compatible();
  bool is_safe = is_ui_thread_safe_;
  bool affects_transparency = current_layer().affects_transparent_layer;
  bool root_has_backdrop_filter = current_layer().contains_backdrop_filter;
  DlBlendMode max_root_blend_mode = current_layer().max_blend_mode;

  sk_sp<DlRTree> rtree;
  SkRect bounds;
  if (rtree_data_.has_value()) {
    auto& rects = rtree_data_->rects;
    auto& indices = rtree_data_->indices;
    rtree = sk_make_sp<DlRTree>(rects.data(), rects.size(), indices.data(),
                                [](int id) { return id >= 0; });
    // RTree bounds may be tighter due to applying filter bounds
    // adjustments to each op as we restore layers rather than to
    // the entire layer bounds.
    bounds = rtree->bounds();
    rtree_data_.reset();
  } else {
    bounds = current_layer().global_space_accumulator.bounds();
  }

  used_ = allocated_ = render_op_count_ = op_index_ = 0;
  nested_bytes_ = nested_op_count_ = 0;
  depth_ = 0;
  is_ui_thread_safe_ = true;
  current_opacity_compatibility_ = true;
  render_op_depth_cost_ = 1u;
  current_ = DlPaint();

  save_stack_.pop_back();
  Init(rtree != nullptr);

  storage_.realloc(bytes);
  return sk_sp<DisplayList>(new DisplayList(
      std::move(storage_), bytes, count, nested_bytes, nested_count,
      total_depth, bounds, opacity_compatible, is_safe, affects_transparency,
      max_root_blend_mode, root_has_backdrop_filter, std::move(rtree)));
}

static constexpr DlRect kEmpty = DlRect();

static const DlRect& ProtectEmpty(const SkRect& rect) {
  // isEmpty protects us against NaN while we normalize any empty cull rects
  return rect.isEmpty() ? kEmpty : ToDlRect(rect);
}

DisplayListBuilder::DisplayListBuilder(const SkRect& cull_rect,
                                       bool prepare_rtree)
    : original_cull_rect_(ProtectEmpty(cull_rect)) {
  Init(prepare_rtree);
}

void DisplayListBuilder::Init(bool prepare_rtree) {
  FML_DCHECK(save_stack_.empty());
  FML_DCHECK(!rtree_data_.has_value());

  save_stack_.emplace_back(original_cull_rect_);
  current_info().is_nop = original_cull_rect_.IsEmpty();
  if (prepare_rtree) {
    rtree_data_.emplace();
  }
}

DisplayListBuilder::~DisplayListBuilder() {
  uint8_t* ptr = storage_.get();
  if (ptr) {
    DisplayList::DisposeOps(ptr, ptr + used_);
  }
}

SkISize DisplayListBuilder::GetBaseLayerSize() const {
  return ToSkISize(DlIRect::RoundOut(original_cull_rect_).GetSize());
}

SkImageInfo DisplayListBuilder::GetImageInfo() const {
  SkISize size = GetBaseLayerSize();
  return SkImageInfo::MakeUnknown(size.width(), size.height());
}

void DisplayListBuilder::onSetAntiAlias(bool aa) {
  current_.setAntiAlias(aa);
  Push<SetAntiAliasOp>(0, aa);
}
void DisplayListBuilder::onSetInvertColors(bool invert) {
  current_.setInvertColors(invert);
  Push<SetInvertColorsOp>(0, invert);
  UpdateCurrentOpacityCompatibility();
}
void DisplayListBuilder::onSetStrokeCap(DlStrokeCap cap) {
  current_.setStrokeCap(cap);
  Push<SetStrokeCapOp>(0, cap);
}
void DisplayListBuilder::onSetStrokeJoin(DlStrokeJoin join) {
  current_.setStrokeJoin(join);
  Push<SetStrokeJoinOp>(0, join);
}
void DisplayListBuilder::onSetDrawStyle(DlDrawStyle style) {
  current_.setDrawStyle(style);
  Push<SetStyleOp>(0, style);
}
void DisplayListBuilder::onSetStrokeWidth(float width) {
  current_.setStrokeWidth(width);
  Push<SetStrokeWidthOp>(0, width);
}
void DisplayListBuilder::onSetStrokeMiter(float limit) {
  current_.setStrokeMiter(limit);
  Push<SetStrokeMiterOp>(0, limit);
}
void DisplayListBuilder::onSetColor(DlColor color) {
  current_.setColor(color);
  Push<SetColorOp>(0, color);
}
void DisplayListBuilder::onSetBlendMode(DlBlendMode mode) {
  current_.setBlendMode(mode);
  Push<SetBlendModeOp>(0, mode);
  UpdateCurrentOpacityCompatibility();
}

void DisplayListBuilder::onSetColorSource(const DlColorSource* source) {
  if (source == nullptr) {
    current_.setColorSource(nullptr);
    Push<ClearColorSourceOp>(0);
  } else {
    current_.setColorSource(source->shared());
    is_ui_thread_safe_ = is_ui_thread_safe_ && source->isUIThreadSafe();
    switch (source->type()) {
      case DlColorSourceType::kColor: {
        const DlColorColorSource* color_source = source->asColor();
        current_.setColorSource(nullptr);
        setColor(color_source->color());
        break;
      }
      case DlColorSourceType::kImage: {
        const DlImageColorSource* image_source = source->asImage();
        FML_DCHECK(image_source);
        Push<SetImageColorSourceOp>(0, image_source);
        break;
      }
      case DlColorSourceType::kLinearGradient: {
        const DlLinearGradientColorSource* linear = source->asLinearGradient();
        FML_DCHECK(linear);
        void* pod = Push<SetPodColorSourceOp>(linear->size());
        new (pod) DlLinearGradientColorSource(linear);
        break;
      }
      case DlColorSourceType::kRadialGradient: {
        const DlRadialGradientColorSource* radial = source->asRadialGradient();
        FML_DCHECK(radial);
        void* pod = Push<SetPodColorSourceOp>(radial->size());
        new (pod) DlRadialGradientColorSource(radial);
        break;
      }
      case DlColorSourceType::kConicalGradient: {
        const DlConicalGradientColorSource* conical =
            source->asConicalGradient();
        FML_DCHECK(conical);
        void* pod = Push<SetPodColorSourceOp>(conical->size());
        new (pod) DlConicalGradientColorSource(conical);
        break;
      }
      case DlColorSourceType::kSweepGradient: {
        const DlSweepGradientColorSource* sweep = source->asSweepGradient();
        FML_DCHECK(sweep);
        void* pod = Push<SetPodColorSourceOp>(sweep->size());
        new (pod) DlSweepGradientColorSource(sweep);
        break;
      }
      case DlColorSourceType::kRuntimeEffect: {
        const DlRuntimeEffectColorSource* effect = source->asRuntimeEffect();
        FML_DCHECK(effect);
        Push<SetRuntimeEffectColorSourceOp>(0, effect);
        break;
      }
#ifdef IMPELLER_ENABLE_3D
      case DlColorSourceType::kScene: {
        const DlSceneColorSource* scene = source->asScene();
        FML_DCHECK(scene);
        Push<SetSceneColorSourceOp>(0, scene);
        break;
      }
#endif  // IMPELLER_ENABLE_3D
    }
  }
}
void DisplayListBuilder::onSetImageFilter(const DlImageFilter* filter) {
  if (filter == nullptr) {
    current_.setImageFilter(nullptr);
    Push<ClearImageFilterOp>(0);
  } else {
    current_.setImageFilter(filter->shared());
    switch (filter->type()) {
      case DlImageFilterType::kBlur: {
        const DlBlurImageFilter* blur_filter = filter->asBlur();
        FML_DCHECK(blur_filter);
        void* pod = Push<SetPodImageFilterOp>(blur_filter->size());
        new (pod) DlBlurImageFilter(blur_filter);
        break;
      }
      case DlImageFilterType::kDilate: {
        const DlDilateImageFilter* dilate_filter = filter->asDilate();
        FML_DCHECK(dilate_filter);
        void* pod = Push<SetPodImageFilterOp>(dilate_filter->size());
        new (pod) DlDilateImageFilter(dilate_filter);
        break;
      }
      case DlImageFilterType::kErode: {
        const DlErodeImageFilter* erode_filter = filter->asErode();
        FML_DCHECK(erode_filter);
        void* pod = Push<SetPodImageFilterOp>(erode_filter->size());
        new (pod) DlErodeImageFilter(erode_filter);
        break;
      }
      case DlImageFilterType::kMatrix: {
        const DlMatrixImageFilter* matrix_filter = filter->asMatrix();
        FML_DCHECK(matrix_filter);
        void* pod = Push<SetPodImageFilterOp>(matrix_filter->size());
        new (pod) DlMatrixImageFilter(matrix_filter);
        break;
      }
      case DlImageFilterType::kCompose:
      case DlImageFilterType::kLocalMatrix:
      case DlImageFilterType::kColorFilter: {
        Push<SetSharedImageFilterOp>(0, filter);
        break;
      }
    }
  }
}
void DisplayListBuilder::onSetColorFilter(const DlColorFilter* filter) {
  if (filter == nullptr) {
    current_.setColorFilter(nullptr);
    Push<ClearColorFilterOp>(0);
  } else {
    current_.setColorFilter(filter->shared());
    switch (filter->type()) {
      case DlColorFilterType::kBlend: {
        const DlBlendColorFilter* blend_filter = filter->asBlend();
        FML_DCHECK(blend_filter);
        void* pod = Push<SetPodColorFilterOp>(blend_filter->size());
        new (pod) DlBlendColorFilter(blend_filter);
        break;
      }
      case DlColorFilterType::kMatrix: {
        const DlMatrixColorFilter* matrix_filter = filter->asMatrix();
        FML_DCHECK(matrix_filter);
        void* pod = Push<SetPodColorFilterOp>(matrix_filter->size());
        new (pod) DlMatrixColorFilter(matrix_filter);
        break;
      }
      case DlColorFilterType::kSrgbToLinearGamma: {
        void* pod = Push<SetPodColorFilterOp>(filter->size());
        new (pod) DlSrgbToLinearGammaColorFilter();
        break;
      }
      case DlColorFilterType::kLinearToSrgbGamma: {
        void* pod = Push<SetPodColorFilterOp>(filter->size());
        new (pod) DlLinearToSrgbGammaColorFilter();
        break;
      }
    }
  }
  UpdateCurrentOpacityCompatibility();
}
void DisplayListBuilder::onSetMaskFilter(const DlMaskFilter* filter) {
  if (filter == nullptr) {
    current_.setMaskFilter(nullptr);
    render_op_depth_cost_ = 1u;
    Push<ClearMaskFilterOp>(0);
  } else {
    current_.setMaskFilter(filter->shared());
    render_op_depth_cost_ = 2u;
    switch (filter->type()) {
      case DlMaskFilterType::kBlur: {
        const DlBlurMaskFilter* blur_filter = filter->asBlur();
        FML_DCHECK(blur_filter);
        void* pod = Push<SetPodMaskFilterOp>(blur_filter->size());
        new (pod) DlBlurMaskFilter(blur_filter);
        break;
      }
    }
  }
}

void DisplayListBuilder::SetAttributesFromPaint(
    const DlPaint& paint,
    const DisplayListAttributeFlags flags) {
  if (flags.applies_anti_alias()) {
    setAntiAlias(paint.isAntiAlias());
  }
  if (flags.applies_alpha_or_color()) {
    setColor(paint.getColor());
  }
  if (flags.applies_blend()) {
    setBlendMode(paint.getBlendMode());
  }
  if (flags.applies_style()) {
    setDrawStyle(paint.getDrawStyle());
  }
  if (flags.is_stroked(paint.getDrawStyle())) {
    setStrokeWidth(paint.getStrokeWidth());
    setStrokeMiter(paint.getStrokeMiter());
    setStrokeCap(paint.getStrokeCap());
    setStrokeJoin(paint.getStrokeJoin());
  }
  if (flags.applies_shader()) {
    setColorSource(paint.getColorSource().get());
  }
  if (flags.applies_color_filter()) {
    setInvertColors(paint.isInvertColors());
    setColorFilter(paint.getColorFilter().get());
  }
  if (flags.applies_image_filter()) {
    setImageFilter(paint.getImageFilter().get());
  }
  if (flags.applies_mask_filter()) {
    setMaskFilter(paint.getMaskFilter().get());
  }
}

void DisplayListBuilder::checkForDeferredSave() {
  if (current_info().has_deferred_save_op) {
    size_t save_offset = used_;
    Push<SaveOp>(0);
    current_info().save_offset = save_offset;
    current_info().save_depth = depth_;
    current_info().has_deferred_save_op = false;
  }
}

void DisplayListBuilder::Save() {
  bool was_nop = current_info().is_nop;
  save_stack_.emplace_back(&current_info());
  current_info().is_nop = was_nop;

  FML_DCHECK(save_stack_.size() >= 2u);
  FML_DCHECK(current_info().has_deferred_save_op);
}

void DisplayListBuilder::saveLayer(const SkRect& bounds,
                                   const SaveLayerOptions in_options,
                                   const DlImageFilter* backdrop) {
  SaveLayerOptions options = in_options.without_optimizations();
  DisplayListAttributeFlags flags = options.renders_with_attributes()
                                        ? kSaveLayerWithPaintFlags
                                        : kSaveLayerFlags;
  OpResult result = PaintResult(current_, flags);
  if (result == OpResult::kNoEffect) {
    // If we can't render, whether because we were already in a no-render
    // state from the parent or because our own attributes make us a nop,
    // we can just simplify this whole layer to a regular save that has
    // nop state. We need to have a SaveInfo for the eventual restore(),
    // but no rendering ops should be accepted between now and then so
    // it doesn't need any of the data associated with a layer SaveInfo.
    Save();
    current_info().is_nop = true;
    return;
  }

  if (backdrop != nullptr) {
    current_layer().contains_backdrop_filter = true;
  }

  // Snapshot these values before we do any work as we need the values
  // from before the method was called, but some of the operations below
  // might update them.
  size_t save_offset = used_;
  uint32_t save_depth = depth_;

  // A backdrop will affect up to the entire surface, bounded by the clip
  bool will_be_unbounded = (backdrop != nullptr);
  std::shared_ptr<const DlImageFilter> filter;

  if (options.renders_with_attributes()) {
    if (!paint_nops_on_transparency()) {
      // We will fill the clip of the outer layer when we restore.
      will_be_unbounded = true;
    }
    filter = current_.getImageFilter();
    CheckLayerOpacityCompatibility(true);
    UpdateLayerResult(result, true);
  } else {
    CheckLayerOpacityCompatibility(false);
    UpdateLayerResult(result, false);
  }

  // The actual flood of the outer layer clip will occur after the
  // (eventual) corresponding restore is called, but rather than
  // remember this information in the LayerInfo until the restore
  // method is processed, we just mark the unbounded state up front.
  // Another reason to accumulate the clip here rather than in
  // restore is so that this savelayer will be tagged in the rtree
  // with its full bounds and the right op_index so that it doesn't
  // get culled during rendering.
  if (will_be_unbounded) {
    // Accumulate should always return true here because if the
    // clip was empty then that would have been caught up above
    // when we tested the PaintResult.
    [[maybe_unused]] bool unclipped = AccumulateUnbounded();
    FML_DCHECK(unclipped);
  }

  // Accumulate information for the SaveInfo we are about to push onto the
  // stack.
  {
    size_t rtree_index =
        rtree_data_.has_value() ? rtree_data_->rects.size() : 0u;

    save_stack_.emplace_back(&current_info(), filter, rtree_index);
    FML_DCHECK(current_info().is_save_layer);
    FML_DCHECK(!current_info().is_nop);
    FML_DCHECK(!current_info().has_deferred_save_op);
    current_info().save_offset = save_offset;
    current_info().save_depth = save_depth;

    // If we inherit some culling bounds and we have a filter then we need
    // to adjust them so that we cull for the correct input space for the
    // output of the filter.
    if (filter) {
      SkRect outer_cull_rect = current_info().global_state.device_cull_rect();
      SkMatrix matrix = current_info().global_state.matrix_3x3();

      SkIRect output_bounds = outer_cull_rect.roundOut();
      SkIRect input_bounds;
      if (filter->get_input_device_bounds(output_bounds, matrix,
                                          input_bounds)) {
        current_info().global_state.resetDeviceCullRect(
            SkRect::Make(input_bounds));
      } else {
        // Filter could not make any promises about the bounds it needs to
        // fill the output space, so we use a maximal rect to accumulate
        // the layer bounds.
        current_info().global_state.resetDeviceCullRect(kMaxCullRect);
      }
    }

    // We always want to cull based on user provided bounds, though, as
    // that is legacy behavior even if it doesn't always work precisely
    // in a rotated or skewed coordinate system (but it will work
    // conservatively).
    if (in_options.bounds_from_caller()) {
      current_info().global_state.clipRect(bounds, ClipOp::kIntersect, false);
    }
  }

  // Accumulate options to store in the SaveLayer op record.
  {
    SkRect record_bounds;
    if (in_options.bounds_from_caller()) {
      options = options.with_bounds_from_caller();
      record_bounds = bounds;
    } else {
      FML_DCHECK(record_bounds.isEmpty());
    }

    if (backdrop) {
      Push<SaveLayerBackdropOp>(0, options, record_bounds, backdrop);
    } else {
      Push<SaveLayerOp>(0, options, record_bounds);
    }
  }

  if (options.renders_with_attributes()) {
    // |current_opacity_compatibility_| does not take an ImageFilter into
    // account because an individual primitive with an ImageFilter can apply
    // opacity on top of it. But, if the layer is applying the ImageFilter
    // then it cannot pass the opacity on.
    if (!current_opacity_compatibility_ || filter) {
      UpdateLayerOpacityCompatibility(false);
    }
  }
}
void DisplayListBuilder::SaveLayer(const SkRect* bounds,
                                   const DlPaint* paint,
                                   const DlImageFilter* backdrop) {
  SaveLayerOptions options;
  SkRect temp_bounds;
  if (bounds) {
    options = options.with_bounds_from_caller();
    temp_bounds = *bounds;
  } else {
    temp_bounds.setEmpty();
  }
  if (paint != nullptr) {
    options = options.with_renders_with_attributes();
    SetAttributesFromPaint(*paint,
                           DisplayListOpFlags::kSaveLayerWithPaintFlags);
  }
  saveLayer(temp_bounds, options, backdrop);
}

void DisplayListBuilder::Restore() {
  if (save_stack_.size() <= 1) {
    return;
  }

  if (!current_info().has_deferred_save_op) {
    SaveOpBase* op = reinterpret_cast<SaveOpBase*>(storage_.get() +
                                                   current_info().save_offset);
    FML_CHECK(op->type == DisplayListOpType::kSave ||
              op->type == DisplayListOpType::kSaveLayer ||
              op->type == DisplayListOpType::kSaveLayerBackdrop);

    op->restore_index = op_index_;
    op->total_content_depth = depth_ - current_info().save_depth;

    if (current_info().is_save_layer) {
      RestoreLayer();
    }

    // Wait until all outgoing bounds information for the saveLayer is
    // recorded before pushing the record to the buffer so that any rtree
    // bounds will be attributed to the op_index of the restore op.
    Push<RestoreOp>(0);
  } else {
    FML_DCHECK(!current_info().is_save_layer);
  }

  save_stack_.pop_back();
}

void DisplayListBuilder::RestoreLayer() {
  FML_DCHECK(save_stack_.size() > 1);
  FML_DCHECK(current_info().is_save_layer);
  FML_DCHECK(!current_info().has_deferred_save_op);

  // A saveLayer will usually do a final copy to the main buffer in
  // addition to its content, but that is accounted for outside of
  // the total content depth computed above in Restore.
  depth_ += render_op_depth_cost_;

  SkRect content_bounds = current_layer().layer_local_accumulator.bounds();

  SaveLayerOpBase* layer_op = reinterpret_cast<SaveLayerOpBase*>(
      storage_.get() + current_info().save_offset);
  FML_CHECK(layer_op->type == DisplayListOpType::kSaveLayer ||
            layer_op->type == DisplayListOpType::kSaveLayerBackdrop);

  if (layer_op->options.bounds_from_caller()) {
    if (!content_bounds.isEmpty() && !layer_op->rect.contains(content_bounds)) {
      layer_op->options = layer_op->options.with_content_is_clipped();
      content_bounds.intersect(layer_op->rect);
    }
  }
  layer_op->rect = content_bounds;
  layer_op->max_blend_mode = current_layer().max_blend_mode;

  if (current_layer().contains_backdrop_filter) {
    layer_op->options = layer_op->options.with_contains_backdrop_filter();
  }

  if (current_layer().is_group_opacity_compatible()) {
    layer_op->options = layer_op->options.with_can_distribute_opacity();
  }

  // Ensure that the bounds transferred in the following call will be
  // attributed to the index of the restore op.
  FML_DCHECK(layer_op->restore_index == op_index_);
  TransferLayerBounds(content_bounds);
}

// There are a few different conditions and corresponding operations to
// consider when transferring bounds from one layer to another. The current
// layer will have accumulated its bounds into 2 potential places:
//
// - Its own private layer local bounds, which were potentially clipped by
//   the supplied bounds and passed here as the content_bounds.
//
// - Either the rtree rect list, or the global space accumulator, one or
//   the other.
//
// If there is no filter then the private layer bounds are complete and
// they simply need to be passed along to the parent into its layer local
// accumulator. Also, if there was no filter then the existing bounds
// recorded in either the rtree rects or the layer's global space accumulator
// (shared with its parent) need no updating so no global space transfer
// has to occur.
//
// If there is a filter then the global content bounds will need to be
// adjusted in one of two ways (rtree vs non-rtree):
//
// - If we are accumulating rtree rects then each of the rects accumulated
//   during this current layer will need to be updated by the filter in the
//   global coordinate space in which they were accumulated. In this mode
//   we should never have a global space accumulator on the layer.
//
// - Otherwise we were accumulating global bounds into our own private
//   global space accumulator which need to be adjusted in the global space
//   coordinate system by the filter.
//
// Finally, we will have to adjust the layer's content bounds by the filter
// and accumulate those into the parent layer's local bounds.
void DisplayListBuilder::TransferLayerBounds(const SkRect& content_bounds) {
  auto& filter = current_layer().filter;

  if (!filter) {
    // We either accumulate global bounds into the rtree_data if there
    // is one, or into the global_space_accumulator, but not both.
    FML_DCHECK(!rtree_data_.has_value() ||
               current_layer().global_space_accumulator.is_empty());

    parent_info().AccumulateBoundsLocal(content_bounds);
    parent_layer().global_space_accumulator.accumulate(
        current_layer().global_space_accumulator);
    return;
  }

  bool parent_is_flooded = false;
  SkRect bounds_for_parent = content_bounds;

  // First, let's adjust or transfer the global/rtree bounds by the filter.

  // Matrix and Clip for the filter adjustment are the global values from
  // just before our saveLayer and should still be the current values
  // present in the parent layer.
  const SkRect clip = parent_info().global_state.device_cull_rect();
  const SkMatrix matrix = parent_info().global_state.matrix_3x3();

  if (rtree_data_.has_value()) {
    // Neither current or parent layer should have any global bounds in
    // their accumulator
    FML_DCHECK(current_layer().global_space_accumulator.is_empty());
    FML_DCHECK(parent_layer().global_space_accumulator.is_empty());

    // The rtree rects were accumulated without the bounds modification of
    // the filter applied to the layer so they may fail to trigger on a
    // culled dispatch if their filter "fringes" are in the dispatch scope
    // but their base rendering bounds are not. (Also, they will not
    // contribute fully when we compute the overall bounds of this DL.)
    //
    // To make sure they are rendered in the culled dispatch situation, we
    // revisit all of the RTree rects accumulated during the current layer
    // (indicated by rtree_rects_start_index) and expand them by the filter.

    if (AdjustRTreeRects(rtree_data_.value(), *filter, matrix, clip,
                         current_layer().rtree_rects_start_index)) {
      parent_is_flooded = true;
    }
  } else {
    SkRect global_bounds = current_layer().global_space_accumulator.bounds();
    if (!global_bounds.isEmpty()) {
      SkIRect global_ibounds = global_bounds.roundOut();
      if (!filter->map_device_bounds(global_ibounds, matrix, global_ibounds)) {
        parent_is_flooded = true;
      } else {
        global_bounds.set(global_ibounds);
        if (global_bounds.intersect(clip)) {
          parent_layer().global_space_accumulator.accumulate(global_bounds);
        }
      }
    }
  }

  // Now we visit the layer bounds which are in the layer's local coordinate
  // system must be accumulated into the parent layer's bounds while
  // adjusting them by the layer's local coordinate system (handled by the
  // Accumulate() methods).

  // A filter will happily adjust empty bounds to be non-empty, so we
  // specifically avoid that case here. Also, if we are already planning
  // to flood the parent due to any of the cases above, we don't need to
  // run the filter on the content bounds only to discover the same
  // condition.
  if (!parent_is_flooded && !bounds_for_parent.isEmpty()) {
    if (!filter->map_local_bounds(bounds_for_parent, bounds_for_parent)) {
      parent_is_flooded = true;
    }
  }

  if (parent_is_flooded) {
    // All of the above computations deferred the flooded parent status
    // to here. We need to mark the parent as flooded in both its layer
    // and global accumulators. Note that even though the rtree rects
    // were expanded to the size of the clip above, this method will still
    // add one more rect to the rtree with the op index of the restore
    // command to prevent the saveLayer itself from being elided in the
    // rare case that there are no rendering ops in it, or somehow none
    // of them were chosen by the rtree search (unlikely). The saveLayer
    // must be processed for the parent flood to happen.
    AccumulateUnbounded(parent_info());
  } else {
    parent_info().AccumulateBoundsLocal(bounds_for_parent);
  }
}

bool DisplayListBuilder::AdjustRTreeRects(RTreeData& data,
                                          const DlImageFilter& filter,
                                          const SkMatrix& matrix,
                                          const SkRect& clip,
                                          size_t rect_start_index) {
  auto& rects = data.rects;
  auto& indices = data.indices;
  FML_DCHECK(rects.size() == indices.size());
  int ret = false;
  auto rect_keep = rect_start_index;
  for (size_t i = rect_start_index; i < rects.size(); i++) {
    SkRect bounds = rects[i];
    SkIRect ibounds;
    if (filter.map_device_bounds(bounds.roundOut(), matrix, ibounds)) {
      bounds.set(ibounds);
    } else {
      bounds = clip;
      ret = true;
    }
    if (bounds.intersect(clip)) {
      indices[rect_keep] = indices[i];
      rects[rect_keep] = bounds;
      rect_keep++;
    }
  }
  indices.resize(rect_keep);
  rects.resize(rect_keep);
  return ret;
}

void DisplayListBuilder::RestoreToCount(int restore_count) {
  FML_DCHECK(restore_count <= GetSaveCount());
  while (restore_count < GetSaveCount() && GetSaveCount() > 1) {
    restore();
  }
}

void DisplayListBuilder::Translate(SkScalar tx, SkScalar ty) {
  if (std::isfinite(tx) && std::isfinite(ty) && (tx != 0.0 || ty != 0.0)) {
    checkForDeferredSave();
    Push<TranslateOp>(0, tx, ty);
    global_state().translate(tx, ty);
    layer_local_state().translate(tx, ty);
  }
}
void DisplayListBuilder::Scale(SkScalar sx, SkScalar sy) {
  if (std::isfinite(sx) && std::isfinite(sy) && (sx != 1.0 || sy != 1.0)) {
    checkForDeferredSave();
    Push<ScaleOp>(0, sx, sy);
    global_state().scale(sx, sy);
    layer_local_state().scale(sx, sy);
  }
}
void DisplayListBuilder::Rotate(SkScalar degrees) {
  if (SkScalarMod(degrees, 360.0) != 0.0) {
    checkForDeferredSave();
    Push<RotateOp>(0, degrees);
    global_state().rotate(degrees);
    layer_local_state().rotate(degrees);
  }
}
void DisplayListBuilder::Skew(SkScalar sx, SkScalar sy) {
  if (std::isfinite(sx) && std::isfinite(sy) && (sx != 0.0 || sy != 0.0)) {
    checkForDeferredSave();
    Push<SkewOp>(0, sx, sy);
    global_state().skew(sx, sy);
    layer_local_state().skew(sx, sy);
  }
}

// clang-format off

// 2x3 2D affine subset of a 4x4 transform in row major order
void DisplayListBuilder::Transform2DAffine(
    SkScalar mxx, SkScalar mxy, SkScalar mxt,
    SkScalar myx, SkScalar myy, SkScalar myt) {
  if (std::isfinite(mxx) && std::isfinite(myx) &&
      std::isfinite(mxy) && std::isfinite(myy) &&
      std::isfinite(mxt) && std::isfinite(myt)) {
    if (mxx == 1 && mxy == 0 &&
        myx == 0 && myy == 1) {
      Translate(mxt, myt);
    } else {
      checkForDeferredSave();
      Push<Transform2DAffineOp>(0,
                                mxx, mxy, mxt,
                                myx, myy, myt);
      global_state().transform2DAffine(mxx, mxy, mxt,
                                       myx, myy, myt);
      layer_local_state().transform2DAffine(mxx, mxy, mxt,
                                            myx, myy, myt);
    }
  }
}
// full 4x4 transform in row major order
void DisplayListBuilder::TransformFullPerspective(
    SkScalar mxx, SkScalar mxy, SkScalar mxz, SkScalar mxt,
    SkScalar myx, SkScalar myy, SkScalar myz, SkScalar myt,
    SkScalar mzx, SkScalar mzy, SkScalar mzz, SkScalar mzt,
    SkScalar mwx, SkScalar mwy, SkScalar mwz, SkScalar mwt) {
  if (                        mxz == 0 &&
                              myz == 0 &&
      mzx == 0 && mzy == 0 && mzz == 1 && mzt == 0 &&
      mwx == 0 && mwy == 0 && mwz == 0 && mwt == 1) {
    Transform2DAffine(mxx, mxy, mxt,
                      myx, myy, myt);
  } else if (std::isfinite(mxx) && std::isfinite(mxy) &&
             std::isfinite(mxz) && std::isfinite(mxt) &&
             std::isfinite(myx) && std::isfinite(myy) &&
             std::isfinite(myz) && std::isfinite(myt) &&
             std::isfinite(mzx) && std::isfinite(mzy) &&
             std::isfinite(mzz) && std::isfinite(mzt) &&
             std::isfinite(mwx) && std::isfinite(mwy) &&
             std::isfinite(mwz) && std::isfinite(mwt)) {
    checkForDeferredSave();
    Push<TransformFullPerspectiveOp>(0,
                                     mxx, mxy, mxz, mxt,
                                     myx, myy, myz, myt,
                                     mzx, mzy, mzz, mzt,
                                     mwx, mwy, mwz, mwt);
    global_state().transformFullPerspective(mxx, mxy, mxz, mxt,
                                            myx, myy, myz, myt,
                                            mzx, mzy, mzz, mzt,
                                            mwx, mwy, mwz, mwt);
    layer_local_state().transformFullPerspective(mxx, mxy, mxz, mxt,
                                                 myx, myy, myz, myt,
                                                 mzx, mzy, mzz, mzt,
                                                 mwx, mwy, mwz, mwt);
  }
}
// clang-format on
void DisplayListBuilder::TransformReset() {
  checkForDeferredSave();
  Push<TransformResetOp>(0);

  // The matrices in layer_tracker_ and tracker_ are similar, but
  // start at a different base transform. The tracker_ potentially
  // has some number of transform operations on it that prefix the
  // operations accumulated in layer_tracker_. So we can't set them both
  // to identity in parallel as they would no longer maintain their
  // relationship to each other.
  // Instead we reinterpret this operation as transforming by the
  // inverse of the current transform. Doing so to tracker_ sets it
  // to identity so we can avoid the math there, but we must do the
  // math the long way for layer_tracker_. This becomes:
  //   layer_tracker_.transform(tracker_.inverse());
  if (!layer_local_state().inverseTransform(global_state())) {
    // If the inverse operation failed then that means that either
    // the matrix above the current layer was singular, or the matrix
    // became singular while we were accumulating the current layer.
    // In either case, we should no longer be accumulating any
    // contents so we set the layer tracking transform to a singular one.
    layer_local_state().setTransform(SkMatrix::Scale(0.0f, 0.0f));
  }

  global_state().setIdentity();
}
void DisplayListBuilder::Transform(const SkMatrix* matrix) {
  if (matrix != nullptr) {
    Transform(SkM44(*matrix));
  }
}
void DisplayListBuilder::Transform(const SkM44* m44) {
  if (m44 != nullptr) {
    transformFullPerspective(
        m44->rc(0, 0), m44->rc(0, 1), m44->rc(0, 2), m44->rc(0, 3),
        m44->rc(1, 0), m44->rc(1, 1), m44->rc(1, 2), m44->rc(1, 3),
        m44->rc(2, 0), m44->rc(2, 1), m44->rc(2, 2), m44->rc(2, 3),
        m44->rc(3, 0), m44->rc(3, 1), m44->rc(3, 2), m44->rc(3, 3));
  }
}

void DisplayListBuilder::ClipRect(const SkRect& rect,
                                  ClipOp clip_op,
                                  bool is_aa) {
  if (!rect.isFinite()) {
    return;
  }
  if (current_info().is_nop) {
    return;
  }
  if (current_info().has_valid_clip &&
      clip_op == DlCanvas::ClipOp::kIntersect &&
      layer_local_state().rect_covers_cull(rect)) {
    return;
  }
  global_state().clipRect(rect, clip_op, is_aa);
  layer_local_state().clipRect(rect, clip_op, is_aa);
  if (global_state().is_cull_rect_empty() ||
      layer_local_state().is_cull_rect_empty()) {
    current_info().is_nop = true;
    return;
  }
  current_info().has_valid_clip = true;
  checkForDeferredSave();
  switch (clip_op) {
    case ClipOp::kIntersect:
      Push<ClipIntersectRectOp>(0, rect, is_aa);
      break;
    case ClipOp::kDifference:
      Push<ClipDifferenceRectOp>(0, rect, is_aa);
      break;
  }
}
void DisplayListBuilder::ClipOval(const SkRect& bounds,
                                  ClipOp clip_op,
                                  bool is_aa) {
  if (!bounds.isFinite()) {
    return;
  }
  if (current_info().is_nop) {
    return;
  }
  if (current_info().has_valid_clip &&
      clip_op == DlCanvas::ClipOp::kIntersect &&
      layer_local_state().oval_covers_cull(bounds)) {
    return;
  }
  global_state().clipOval(bounds, clip_op, is_aa);
  layer_local_state().clipOval(bounds, clip_op, is_aa);
  if (global_state().is_cull_rect_empty() ||
      layer_local_state().is_cull_rect_empty()) {
    current_info().is_nop = true;
    return;
  }
  current_info().has_valid_clip = true;
  checkForDeferredSave();
  switch (clip_op) {
    case ClipOp::kIntersect:
      Push<ClipIntersectOvalOp>(0, bounds, is_aa);
      break;
    case ClipOp::kDifference:
      Push<ClipDifferenceOvalOp>(0, bounds, is_aa);
      break;
  }
}
void DisplayListBuilder::ClipRRect(const SkRRect& rrect,
                                   ClipOp clip_op,
                                   bool is_aa) {
  if (current_info().is_nop) {
    return;
  }
  if (rrect.isRect()) {
    clipRect(rrect.rect(), clip_op, is_aa);
    return;
  }
  if (rrect.isOval()) {
    clipOval(rrect.rect(), clip_op, is_aa);
    return;
  }
  if (current_info().has_valid_clip &&
      clip_op == DlCanvas::ClipOp::kIntersect &&
      layer_local_state().rrect_covers_cull(rrect)) {
    return;
  }
  global_state().clipRRect(rrect, clip_op, is_aa);
  layer_local_state().clipRRect(rrect, clip_op, is_aa);
  if (global_state().is_cull_rect_empty() ||
      layer_local_state().is_cull_rect_empty()) {
    current_info().is_nop = true;
    return;
  }
  current_info().has_valid_clip = true;
  checkForDeferredSave();
  switch (clip_op) {
    case ClipOp::kIntersect:
      Push<ClipIntersectRRectOp>(0, rrect, is_aa);
      break;
    case ClipOp::kDifference:
      Push<ClipDifferenceRRectOp>(0, rrect, is_aa);
      break;
  }
}
void DisplayListBuilder::ClipPath(const SkPath& path,
                                  ClipOp clip_op,
                                  bool is_aa) {
  if (current_info().is_nop) {
    return;
  }
  if (!path.isInverseFillType()) {
    SkRect rect;
    if (path.isRect(&rect)) {
      this->clipRect(rect, clip_op, is_aa);
      return;
    }
    if (path.isOval(&rect)) {
      this->clipOval(rect, clip_op, is_aa);
      return;
    }
    SkRRect rrect;
    if (path.isRRect(&rrect)) {
      this->clipRRect(rrect, clip_op, is_aa);
      return;
    }
  }
  global_state().clipPath(path, clip_op, is_aa);
  layer_local_state().clipPath(path, clip_op, is_aa);
  if (global_state().is_cull_rect_empty() ||
      layer_local_state().is_cull_rect_empty()) {
    current_info().is_nop = true;
    return;
  }
  current_info().has_valid_clip = true;
  checkForDeferredSave();
  switch (clip_op) {
    case ClipOp::kIntersect:
      Push<ClipIntersectPathOp>(0, path, is_aa);
      break;
    case ClipOp::kDifference:
      Push<ClipDifferencePathOp>(0, path, is_aa);
      break;
  }
}

bool DisplayListBuilder::QuickReject(const SkRect& bounds) const {
  return global_state().content_culled(bounds);
}

void DisplayListBuilder::drawPaint() {
  OpResult result = PaintResult(current_, kDrawPaintFlags);
  if (result != OpResult::kNoEffect && AccumulateUnbounded()) {
    Push<DrawPaintOp>(0);
    CheckLayerOpacityCompatibility();
    UpdateLayerResult(result);
  }
}
void DisplayListBuilder::DrawPaint(const DlPaint& paint) {
  SetAttributesFromPaint(paint, DisplayListOpFlags::kDrawPaintFlags);
  drawPaint();
}
void DisplayListBuilder::DrawColor(DlColor color, DlBlendMode mode) {
  OpResult result = PaintResult(DlPaint(color).setBlendMode(mode));
  if (result != OpResult::kNoEffect && AccumulateUnbounded()) {
    Push<DrawColorOp>(0, color, mode);
    CheckLayerOpacityCompatibility(mode);
    UpdateLayerResult(result, mode);
  }
}
void DisplayListBuilder::drawLine(const SkPoint& p0, const SkPoint& p1) {
  SkRect bounds = SkRect::MakeLTRB(p0.fX, p0.fY, p1.fX, p1.fY).makeSorted();
  DisplayListAttributeFlags flags =
      (bounds.width() > 0.0f && bounds.height() > 0.0f) ? kDrawLineFlags
                                                        : kDrawHVLineFlags;
  OpResult result = PaintResult(current_, flags);
  if (result != OpResult::kNoEffect && AccumulateOpBounds(bounds, flags)) {
    Push<DrawLineOp>(0, p0, p1);
    CheckLayerOpacityCompatibility();
    UpdateLayerResult(result);
  }
}
void DisplayListBuilder::DrawLine(const SkPoint& p0,
                                  const SkPoint& p1,
                                  const DlPaint& paint) {
  SetAttributesFromPaint(paint, DisplayListOpFlags::kDrawLineFlags);
  drawLine(p0, p1);
}
void DisplayListBuilder::drawDashedLine(const DlPoint& p0,
                                        const DlPoint& p1,
                                        DlScalar on_length,
                                        DlScalar off_length) {
  SkRect bounds = SkRect::MakeLTRB(p0.x, p0.y, p1.x, p1.y).makeSorted();
  DisplayListAttributeFlags flags =
      (bounds.width() > 0.0f && bounds.height() > 0.0f) ? kDrawLineFlags
                                                        : kDrawHVLineFlags;
  OpResult result = PaintResult(current_, flags);
  if (result != OpResult::kNoEffect && AccumulateOpBounds(bounds, flags)) {
    Push<DrawDashedLineOp>(0, p0, p1, on_length, off_length);
    CheckLayerOpacityCompatibility();
    UpdateLayerResult(result);
  }
}
void DisplayListBuilder::DrawDashedLine(const DlPoint& p0,
                                        const DlPoint& p1,
                                        DlScalar on_length,
                                        DlScalar off_length,
                                        const DlPaint& paint) {
  SetAttributesFromPaint(paint, DisplayListOpFlags::kDrawLineFlags);
  drawDashedLine(p0, p1, on_length, off_length);
}
void DisplayListBuilder::drawRect(const SkRect& rect) {
  DisplayListAttributeFlags flags = kDrawRectFlags;
  OpResult result = PaintResult(current_, flags);
  if (result != OpResult::kNoEffect &&
      AccumulateOpBounds(rect.makeSorted(), flags)) {
    Push<DrawRectOp>(0, rect);
    CheckLayerOpacityCompatibility();
    UpdateLayerResult(result);
  }
}
void DisplayListBuilder::DrawRect(const SkRect& rect, const DlPaint& paint) {
  SetAttributesFromPaint(paint, DisplayListOpFlags::kDrawRectFlags);
  drawRect(rect);
}
void DisplayListBuilder::drawOval(const SkRect& bounds) {
  DisplayListAttributeFlags flags = kDrawOvalFlags;
  OpResult result = PaintResult(current_, flags);
  if (result != OpResult::kNoEffect &&
      AccumulateOpBounds(bounds.makeSorted(), flags)) {
    Push<DrawOvalOp>(0, bounds);
    CheckLayerOpacityCompatibility();
    UpdateLayerResult(result);
  }
}
void DisplayListBuilder::DrawOval(const SkRect& bounds, const DlPaint& paint) {
  SetAttributesFromPaint(paint, DisplayListOpFlags::kDrawOvalFlags);
  drawOval(bounds);
}
void DisplayListBuilder::drawCircle(const SkPoint& center, SkScalar radius) {
  DisplayListAttributeFlags flags = kDrawCircleFlags;
  OpResult result = PaintResult(current_, flags);
  if (result != OpResult::kNoEffect) {
    SkRect bounds = SkRect::MakeLTRB(center.fX - radius, center.fY - radius,
                                     center.fX + radius, center.fY + radius);
    if (AccumulateOpBounds(bounds, flags)) {
      Push<DrawCircleOp>(0, center, radius);
      CheckLayerOpacityCompatibility();
      UpdateLayerResult(result);
    }
  }
}
void DisplayListBuilder::DrawCircle(const SkPoint& center,
                                    SkScalar radius,
                                    const DlPaint& paint) {
  SetAttributesFromPaint(paint, DisplayListOpFlags::kDrawCircleFlags);
  drawCircle(center, radius);
}
void DisplayListBuilder::drawRRect(const SkRRect& rrect) {
  if (rrect.isRect()) {
    drawRect(rrect.rect());
  } else if (rrect.isOval()) {
    drawOval(rrect.rect());
  } else {
    DisplayListAttributeFlags flags = kDrawRRectFlags;
    OpResult result = PaintResult(current_, flags);
    if (result != OpResult::kNoEffect &&
        AccumulateOpBounds(rrect.getBounds(), flags)) {
      Push<DrawRRectOp>(0, rrect);
      CheckLayerOpacityCompatibility();
      UpdateLayerResult(result);
    }
  }
}
void DisplayListBuilder::DrawRRect(const SkRRect& rrect, const DlPaint& paint) {
  SetAttributesFromPaint(paint, DisplayListOpFlags::kDrawRRectFlags);
  drawRRect(rrect);
}
void DisplayListBuilder::drawDRRect(const SkRRect& outer,
                                    const SkRRect& inner) {
  DisplayListAttributeFlags flags = kDrawDRRectFlags;
  OpResult result = PaintResult(current_, flags);
  if (result != OpResult::kNoEffect &&
      AccumulateOpBounds(outer.getBounds(), flags)) {
    Push<DrawDRRectOp>(0, outer, inner);
    CheckLayerOpacityCompatibility();
    UpdateLayerResult(result);
  }
}
void DisplayListBuilder::DrawDRRect(const SkRRect& outer,
                                    const SkRRect& inner,
                                    const DlPaint& paint) {
  SetAttributesFromPaint(paint, DisplayListOpFlags::kDrawDRRectFlags);
  drawDRRect(outer, inner);
}
void DisplayListBuilder::drawPath(const SkPath& path) {
  {
    SkRect rect;
    if (path.isRect(&rect)) {
      drawRect(rect);
      return;
    }
    if (path.isOval(&rect)) {
      drawOval(rect);
      return;
    }
  }
  {
    SkRRect rrect;
    if (path.isRRect(&rrect)) {
      drawRRect(rrect);
      return;
    }
  }
  DisplayListAttributeFlags flags = kDrawPathFlags;
  OpResult result = PaintResult(current_, flags);
  if (result != OpResult::kNoEffect) {
    bool is_visible = path.isInverseFillType()
                          ? AccumulateUnbounded()
                          : AccumulateOpBounds(path.getBounds(), flags);
    if (is_visible) {
      Push<DrawPathOp>(0, path);
      CheckLayerOpacityHairlineCompatibility();
      UpdateLayerResult(result);
    }
  }
}
void DisplayListBuilder::DrawPath(const SkPath& path, const DlPaint& paint) {
  SetAttributesFromPaint(paint, DisplayListOpFlags::kDrawPathFlags);
  drawPath(path);
}

void DisplayListBuilder::drawArc(const SkRect& bounds,
                                 SkScalar start,
                                 SkScalar sweep,
                                 bool useCenter) {
  DisplayListAttributeFlags flags =  //
      useCenter                      //
          ? kDrawArcWithCenterFlags
          : kDrawArcNoCenterFlags;
  OpResult result = PaintResult(current_, flags);
  // This could be tighter if we compute where the start and end
  // angles are and then also consider the quadrants swept and
  // the center if specified.
  if (result != OpResult::kNoEffect && AccumulateOpBounds(bounds, flags)) {
    Push<DrawArcOp>(0, bounds, start, sweep, useCenter);
    if (useCenter) {
      CheckLayerOpacityHairlineCompatibility();
    } else {
      CheckLayerOpacityCompatibility();
    }
    UpdateLayerResult(result);
  }
}
void DisplayListBuilder::DrawArc(const SkRect& bounds,
                                 SkScalar start,
                                 SkScalar sweep,
                                 bool useCenter,
                                 const DlPaint& paint) {
  SetAttributesFromPaint(
      paint, useCenter ? kDrawArcWithCenterFlags : kDrawArcNoCenterFlags);
  drawArc(bounds, start, sweep, useCenter);
}

DisplayListAttributeFlags DisplayListBuilder::FlagsForPointMode(
    PointMode mode) {
  switch (mode) {
    case DlCanvas::PointMode::kPoints:
      return kDrawPointsAsPointsFlags;
    case PointMode::kLines:
      return kDrawPointsAsLinesFlags;
    case PointMode::kPolygon:
      return kDrawPointsAsPolygonFlags;
  }
  FML_UNREACHABLE();
}
void DisplayListBuilder::drawPoints(PointMode mode,
                                    uint32_t count,
                                    const SkPoint pts[]) {
  if (count == 0) {
    return;
  }
  DisplayListAttributeFlags flags = FlagsForPointMode(mode);
  OpResult result = PaintResult(current_, flags);
  if (result == OpResult::kNoEffect) {
    return;
  }

  FML_DCHECK(count < DlOpReceiver::kMaxDrawPointsCount);
  int bytes = count * sizeof(SkPoint);
  AccumulationRect accumulator;
  for (size_t i = 0; i < count; i++) {
    accumulator.accumulate(pts[i]);
  }
  SkRect point_bounds = accumulator.bounds();
  if (!AccumulateOpBounds(point_bounds, flags)) {
    return;
  }

  void* data_ptr;
  switch (mode) {
    case PointMode::kPoints:
      data_ptr = Push<DrawPointsOp>(bytes, count);
      break;
    case PointMode::kLines:
      data_ptr = Push<DrawLinesOp>(bytes, count);
      break;
    case PointMode::kPolygon:
      data_ptr = Push<DrawPolygonOp>(bytes, count);
      break;
    default:
      FML_UNREACHABLE();
      return;
  }
  CopyV(data_ptr, pts, count);
  // drawPoints treats every point or line (or segment of a polygon)
  // as a completely separate operation meaning we cannot ensure
  // distribution of group opacity without analyzing the mode and the
  // bounds of every sub-primitive.
  // See: https://fiddle.skia.org/c/228459001d2de8db117ce25ef5cedb0c
  current_layer().layer_local_accumulator.record_overlapping_bounds();
  // Even though we've eliminated the possibility of opacity peephole
  // optimizations above, we still set the appropriate flags based on
  // the rendering attributes in case we solve the overlapping points
  // problem above.
  CheckLayerOpacityCompatibility();
  UpdateLayerResult(result);
}
void DisplayListBuilder::DrawPoints(PointMode mode,
                                    uint32_t count,
                                    const SkPoint pts[],
                                    const DlPaint& paint) {
  SetAttributesFromPaint(paint, FlagsForPointMode(mode));
  drawPoints(mode, count, pts);
}
void DisplayListBuilder::drawVertices(
    const std::shared_ptr<DlVertices>& vertices,
    DlBlendMode mode) {
  DisplayListAttributeFlags flags = kDrawVerticesFlags;
  OpResult result = PaintResult(current_, flags);
  if (result != OpResult::kNoEffect &&
      AccumulateOpBounds(vertices->bounds(), flags)) {
    Push<DrawVerticesOp>(0, vertices, mode);
    // DrawVertices applies its colors to the paint so we have no way
    // of controlling opacity using the current paint attributes.
    // Although, examination of the |mode| might find some predictable
    // cases.
    UpdateLayerOpacityCompatibility(false);
    UpdateLayerResult(result);
    // Even though we already eliminated opacity peephole optimization
    // due to the color issues identified above, drawVertices also fails
    // based on the fact that the vertices are rendered independently
    // so we cannot guarantee the non-overlapping condition. We record
    // both conditions in case a solution is found to applying the
    // colors above - both conditions must be analyzed sufficiently
    // and implemented accordingly before drawVertices is compatible with
    // opacity peephole optimizations.
    current_layer().layer_local_accumulator.record_overlapping_bounds();
  }
}
void DisplayListBuilder::DrawVertices(
    const std::shared_ptr<DlVertices>& vertices,
    DlBlendMode mode,
    const DlPaint& paint) {
  SetAttributesFromPaint(paint, DisplayListOpFlags::kDrawVerticesFlags);
  drawVertices(vertices, mode);
}

void DisplayListBuilder::drawImage(const sk_sp<DlImage> image,
                                   const SkPoint point,
                                   DlImageSampling sampling,
                                   bool render_with_attributes) {
  DisplayListAttributeFlags flags = render_with_attributes  //
                                        ? kDrawImageWithPaintFlags
                                        : kDrawImageFlags;
  OpResult result = PaintResult(current_, flags);
  if (result == OpResult::kNoEffect) {
    return;
  }
  SkRect bounds = SkRect::MakeXYWH(point.fX, point.fY,  //
                                   image->width(), image->height());
  if (AccumulateOpBounds(bounds, flags)) {
    render_with_attributes
        ? Push<DrawImageWithAttrOp>(0, image, point, sampling)
        : Push<DrawImageOp>(0, image, point, sampling);
    CheckLayerOpacityCompatibility(render_with_attributes);
    UpdateLayerResult(result, render_with_attributes);
    is_ui_thread_safe_ = is_ui_thread_safe_ && image->isUIThreadSafe();
  }
}
void DisplayListBuilder::DrawImage(const sk_sp<DlImage>& image,
                                   const SkPoint point,
                                   DlImageSampling sampling,
                                   const DlPaint* paint) {
  if (paint != nullptr) {
    SetAttributesFromPaint(*paint,
                           DisplayListOpFlags::kDrawImageWithPaintFlags);
    drawImage(image, point, sampling, true);
  } else {
    drawImage(image, point, sampling, false);
  }
}
void DisplayListBuilder::drawImageRect(const sk_sp<DlImage> image,
                                       const SkRect& src,
                                       const SkRect& dst,
                                       DlImageSampling sampling,
                                       bool render_with_attributes,
                                       SrcRectConstraint constraint) {
  DisplayListAttributeFlags flags = render_with_attributes
                                        ? kDrawImageRectWithPaintFlags
                                        : kDrawImageRectFlags;
  OpResult result = PaintResult(current_, flags);
  if (result != OpResult::kNoEffect && AccumulateOpBounds(dst, flags)) {
    Push<DrawImageRectOp>(0, image, src, dst, sampling, render_with_attributes,
                          constraint);
    CheckLayerOpacityCompatibility(render_with_attributes);
    UpdateLayerResult(result, render_with_attributes);
    is_ui_thread_safe_ = is_ui_thread_safe_ && image->isUIThreadSafe();
  }
}
void DisplayListBuilder::DrawImageRect(const sk_sp<DlImage>& image,
                                       const SkRect& src,
                                       const SkRect& dst,
                                       DlImageSampling sampling,
                                       const DlPaint* paint,
                                       SrcRectConstraint constraint) {
  if (paint != nullptr) {
    SetAttributesFromPaint(*paint,
                           DisplayListOpFlags::kDrawImageRectWithPaintFlags);
    drawImageRect(image, src, dst, sampling, true, constraint);
  } else {
    drawImageRect(image, src, dst, sampling, false, constraint);
  }
}
void DisplayListBuilder::drawImageNine(const sk_sp<DlImage> image,
                                       const SkIRect& center,
                                       const SkRect& dst,
                                       DlFilterMode filter,
                                       bool render_with_attributes) {
  DisplayListAttributeFlags flags = render_with_attributes
                                        ? kDrawImageNineWithPaintFlags
                                        : kDrawImageNineFlags;
  OpResult result = PaintResult(current_, flags);
  if (result != OpResult::kNoEffect && AccumulateOpBounds(dst, flags)) {
    render_with_attributes
        ? Push<DrawImageNineWithAttrOp>(0, image, center, dst, filter)
        : Push<DrawImageNineOp>(0, image, center, dst, filter);
    CheckLayerOpacityCompatibility(render_with_attributes);
    UpdateLayerResult(result, render_with_attributes);
    is_ui_thread_safe_ = is_ui_thread_safe_ && image->isUIThreadSafe();
  }
}
void DisplayListBuilder::DrawImageNine(const sk_sp<DlImage>& image,
                                       const SkIRect& center,
                                       const SkRect& dst,
                                       DlFilterMode filter,
                                       const DlPaint* paint) {
  if (paint != nullptr) {
    SetAttributesFromPaint(*paint,
                           DisplayListOpFlags::kDrawImageNineWithPaintFlags);
    drawImageNine(image, center, dst, filter, true);
  } else {
    drawImageNine(image, center, dst, filter, false);
  }
}
void DisplayListBuilder::drawAtlas(const sk_sp<DlImage> atlas,
                                   const SkRSXform xform[],
                                   const SkRect tex[],
                                   const DlColor colors[],
                                   int count,
                                   DlBlendMode mode,
                                   DlImageSampling sampling,
                                   const SkRect* cull_rect,
                                   bool render_with_attributes) {
  DisplayListAttributeFlags flags = render_with_attributes  //
                                        ? kDrawAtlasWithPaintFlags
                                        : kDrawAtlasFlags;
  OpResult result = PaintResult(current_, flags);
  if (result == OpResult::kNoEffect) {
    return;
  }
  SkPoint quad[4];
  AccumulationRect accumulator;
  for (int i = 0; i < count; i++) {
    const SkRect& src = tex[i];
    xform[i].toQuad(src.width(), src.height(), quad);
    for (int j = 0; j < 4; j++) {
      accumulator.accumulate(quad[j]);
    }
  }
  if (accumulator.is_empty() ||
      !AccumulateOpBounds(accumulator.bounds(), flags)) {
    return;
  }
  // Accumulating the bounds might not trip the overlap condition if the
  // whole atlas operation is separated from other rendering calls, but
  // since each atlas op is treated as an independent operation, we have
  // to pass along our locally computed overlap condition for the individual
  // atlas operations to the layer accumulator.
  // Note that the above accumulation may falsely trigger the overlapping
  // state as it is done quad corner by quad corner and an entire quad may
  // be non-overlapping with the layer bounds, but as we add each point
  // independently it might expand the bounds on one corner and then flag
  // the condition when the next corner is added.
  if (accumulator.overlap_detected()) {
    current_layer().layer_local_accumulator.record_overlapping_bounds();
  }

  int bytes = count * (sizeof(SkRSXform) + sizeof(SkRect));
  void* data_ptr;
  if (colors != nullptr) {
    bytes += count * sizeof(DlColor);
    if (cull_rect != nullptr) {
      data_ptr =
          Push<DrawAtlasCulledOp>(bytes, atlas, count, mode, sampling, true,
                                  *cull_rect, render_with_attributes);
    } else {
      data_ptr = Push<DrawAtlasOp>(bytes, atlas, count, mode, sampling, true,
                                   render_with_attributes);
    }
    CopyV(data_ptr, xform, count, tex, count, colors, count);
  } else {
    if (cull_rect != nullptr) {
      data_ptr =
          Push<DrawAtlasCulledOp>(bytes, atlas, count, mode, sampling, false,
                                  *cull_rect, render_with_attributes);
    } else {
      data_ptr = Push<DrawAtlasOp>(bytes, atlas, count, mode, sampling, false,
                                   render_with_attributes);
    }
    CopyV(data_ptr, xform, count, tex, count);
  }
  // drawAtlas treats each image as a separate operation so we cannot rely
  // on it to distribute the opacity without overlap without checking all
  // of the transforms and texture rectangles.
  UpdateLayerOpacityCompatibility(false);
  UpdateLayerResult(result, render_with_attributes);
  is_ui_thread_safe_ = is_ui_thread_safe_ && atlas->isUIThreadSafe();
}
void DisplayListBuilder::DrawAtlas(const sk_sp<DlImage>& atlas,
                                   const SkRSXform xform[],
                                   const SkRect tex[],
                                   const DlColor colors[],
                                   int count,
                                   DlBlendMode mode,
                                   DlImageSampling sampling,
                                   const SkRect* cull_rect,
                                   const DlPaint* paint) {
  if (paint != nullptr) {
    SetAttributesFromPaint(*paint,
                           DisplayListOpFlags::kDrawAtlasWithPaintFlags);
    drawAtlas(atlas, xform, tex, colors, count, mode, sampling, cull_rect,
              true);
  } else {
    drawAtlas(atlas, xform, tex, colors, count, mode, sampling, cull_rect,
              false);
  }
}

void DisplayListBuilder::DrawDisplayList(const sk_sp<DisplayList> display_list,
                                         SkScalar opacity) {
  if (!std::isfinite(opacity) || opacity <= SK_ScalarNearlyZero ||
      display_list->op_count() == 0 || display_list->bounds().isEmpty() ||
      current_info().is_nop) {
    return;
  }
  const SkRect bounds = display_list->bounds();
  bool accumulated;
  sk_sp<const DlRTree> rtree;
  if (!rtree_data_.has_value() || !(rtree = display_list->rtree())) {
    accumulated = AccumulateOpBounds(bounds, kDrawDisplayListFlags);
  } else {
    std::list<SkRect> rects =
        rtree->searchAndConsolidateRects(GetLocalClipBounds(), false);
    accumulated = false;
    for (const SkRect& rect : rects) {
      // TODO (https://github.com/flutter/flutter/issues/114919): Attributes
      // are not necessarily `kDrawDisplayListFlags`.
      if (AccumulateOpBounds(rect, kDrawDisplayListFlags)) {
        accumulated = true;
      }
    }
  }
  if (!accumulated) {
    return;
  }

  DlPaint current_paint = current_;
  Push<DrawDisplayListOp>(0, display_list,
                          opacity < SK_Scalar1 ? opacity : SK_Scalar1);

  // This depth increment accounts for every draw call in the child
  // DisplayList and is in addition to the implicit depth increment
  // that was performed when we pushed the DrawDisplayListOp. The
  // eventual dispatcher can use or ignore the implicit depth increment
  // as it sees fit depending on whether it needs to do rendering
  // before or after the drawDisplayList op, but it must be accounted
  // for if the depth value accounting is to remain consistent between
  // the recording and dispatching process.
  depth_ += display_list->total_depth();

  is_ui_thread_safe_ = is_ui_thread_safe_ && display_list->isUIThreadSafe();
  // Not really necessary if the developer is interacting with us via
  // our attribute-state-less DlCanvas methods, but this avoids surprises
  // for those who may have been using the stateful Dispatcher methods.
  SetAttributesFromPaint(current_paint,
                         DisplayListOpFlags::kSaveLayerWithPaintFlags);

  // The non-nested op count accumulated in the |Push| method will include
  // this call to |drawDisplayList| for non-nested op count metrics.
  // But, for nested op count metrics we want the |drawDisplayList| call itself
  // to be transparent. So we subtract 1 from our accumulated nested count to
  // balance out against the 1 that was accumulated into the regular count.
  // This behavior is identical to the way SkPicture computed nested op counts.
  nested_op_count_ += display_list->op_count(true) - 1;
  nested_bytes_ += display_list->bytes(true);
  UpdateLayerOpacityCompatibility(display_list->can_apply_group_opacity());
  // Nop DisplayLists are eliminated above so we either affect transparent
  // pixels or we do not. We should not have [kNoEffect].
  UpdateLayerResult(display_list->modifies_transparent_black()
                        ? OpResult::kAffectsAll
                        : OpResult::kPreservesTransparency,
                    display_list->max_root_blend_mode());
  if (display_list->root_has_backdrop_filter()) {
    current_layer().contains_backdrop_filter = true;
  }
}
void DisplayListBuilder::drawTextBlob(const sk_sp<SkTextBlob> blob,
                                      SkScalar x,
                                      SkScalar y) {
  DisplayListAttributeFlags flags = kDrawTextBlobFlags;
  OpResult result = PaintResult(current_, flags);
  if (result == OpResult::kNoEffect) {
    return;
  }
  bool unclipped = AccumulateOpBounds(blob->bounds().makeOffset(x, y), flags);
  // TODO(https://github.com/flutter/flutter/issues/82202): Remove once the
  // unit tests can use Fuchsia's font manager instead of the empty default.
  // Until then we might encounter empty bounds for otherwise valid text and
  // thus we ignore the results from AccumulateOpBounds.
#if defined(OS_FUCHSIA)
  unclipped = true;
#endif  // OS_FUCHSIA
  if (unclipped) {
    Push<DrawTextBlobOp>(0, blob, x, y);
    // There is no way to query if the glyphs of a text blob overlap and
    // there are no current guarantees from either Skia or Impeller that
    // they will protect overlapping glyphs from the effects of overdraw
    // so we must make the conservative assessment that this DL layer is
    // not compatible with group opacity inheritance.
    UpdateLayerOpacityCompatibility(false);
    UpdateLayerResult(result);
  }
}
void DisplayListBuilder::DrawTextBlob(const sk_sp<SkTextBlob>& blob,
                                      SkScalar x,
                                      SkScalar y,
                                      const DlPaint& paint) {
  SetAttributesFromPaint(paint, DisplayListOpFlags::kDrawTextBlobFlags);
  drawTextBlob(blob, x, y);
}

void DisplayListBuilder::drawTextFrame(
    const std::shared_ptr<impeller::TextFrame>& text_frame,
    SkScalar x,
    SkScalar y) {
  DisplayListAttributeFlags flags = kDrawTextBlobFlags;
  OpResult result = PaintResult(current_, flags);
  if (result == OpResult::kNoEffect) {
    return;
  }
  impeller::Rect bounds = text_frame->GetBounds();
  SkRect sk_bounds = SkRect::MakeLTRB(bounds.GetLeft(), bounds.GetTop(),
                                      bounds.GetRight(), bounds.GetBottom());
  bool unclipped = AccumulateOpBounds(sk_bounds.makeOffset(x, y), flags);
  // TODO(https://github.com/flutter/flutter/issues/82202): Remove once the
  // unit tests can use Fuchsia's font manager instead of the empty default.
  // Until then we might encounter empty bounds for otherwise valid text and
  // thus we ignore the results from AccumulateOpBounds.
#if defined(OS_FUCHSIA)
  unclipped = true;
#endif  // OS_FUCHSIA
  if (unclipped) {
    Push<DrawTextFrameOp>(0, text_frame, x, y);
    // There is no way to query if the glyphs of a text blob overlap and
    // there are no current guarantees from either Skia or Impeller that
    // they will protect overlapping glyphs from the effects of overdraw
    // so we must make the conservative assessment that this DL layer is
    // not compatible with group opacity inheritance.
    UpdateLayerOpacityCompatibility(false);
    UpdateLayerResult(result);
  }
}

void DisplayListBuilder::DrawTextFrame(
    const std::shared_ptr<impeller::TextFrame>& text_frame,
    SkScalar x,
    SkScalar y,
    const DlPaint& paint) {
  SetAttributesFromPaint(paint, DisplayListOpFlags::kDrawTextBlobFlags);
  drawTextFrame(text_frame, x, y);
}

void DisplayListBuilder::DrawShadow(const SkPath& path,
                                    const DlColor color,
                                    const SkScalar elevation,
                                    bool transparent_occluder,
                                    SkScalar dpr) {
  OpResult result = PaintResult(DlPaint(color));
  if (result != OpResult::kNoEffect) {
    SkRect shadow_bounds =
        DlCanvas::ComputeShadowBounds(path, elevation, dpr, GetTransform());
    if (AccumulateOpBounds(shadow_bounds, kDrawShadowFlags)) {
      transparent_occluder  //
          ? Push<DrawShadowTransparentOccluderOp>(0, path, color, elevation,
                                                  dpr)
          : Push<DrawShadowOp>(0, path, color, elevation, dpr);
      UpdateLayerOpacityCompatibility(false);
      UpdateLayerResult(result, DlBlendMode::kSrcOver);
    }
  }
}

bool DisplayListBuilder::AdjustBoundsForPaint(SkRect& bounds,
                                              DisplayListAttributeFlags flags) {
  if (flags.ignores_paint()) {
    return true;
  }

  if (flags.is_geometric()) {
    bool is_stroked = flags.is_stroked(current_.getDrawStyle());

    // Path effect occurs before stroking...
    DisplayListSpecialGeometryFlags special_flags =
        flags.GeometryFlags(is_stroked);

    if (is_stroked) {
      // Determine the max multiplier to the stroke width first.
      SkScalar pad = 1.0f;
      if (current_.getStrokeJoin() == DlStrokeJoin::kMiter &&
          special_flags.may_have_acute_joins()) {
        pad = std::max(pad, current_.getStrokeMiter());
      }
      if (current_.getStrokeCap() == DlStrokeCap::kSquare &&
          special_flags.may_have_diagonal_caps()) {
        pad = std::max(pad, SK_ScalarSqrt2);
      }
      SkScalar min_stroke_width = 0.01;
      pad *= std::max(current_.getStrokeWidth() * 0.5f, min_stroke_width);
      bounds.outset(pad, pad);
    }
  }

  if (flags.applies_mask_filter()) {
    auto filter = current_.getMaskFilter();
    if (filter) {
      switch (filter->type()) {
        case DlMaskFilterType::kBlur: {
          FML_DCHECK(filter->asBlur());
          SkScalar mask_sigma_pad = filter->asBlur()->sigma() * 3.0;
          bounds.outset(mask_sigma_pad, mask_sigma_pad);
        }
      }
    }
  }

  // Color filter does not modify bounds even if it affects transparent
  // black because it is clipped by the "mask" of the primitive. That
  // property only comes into play when it is applied to something like
  // a layer.

  if (flags.applies_image_filter()) {
    auto filter = current_.getImageFilterPtr();
    if (filter && !filter->map_local_bounds(bounds, bounds)) {
      return false;
    }
  }

  return true;
}

bool DisplayListBuilder::AccumulateUnbounded(const SaveInfo& save) {
  SkRect global_clip = save.global_state.device_cull_rect();
  SkRect layer_clip = save.global_state.local_cull_rect();
  if (global_clip.isEmpty() || !save.layer_state.mapAndClipRect(&layer_clip)) {
    return false;
  }
  if (rtree_data_.has_value()) {
    FML_DCHECK(save.layer_info->global_space_accumulator.is_empty());
    rtree_data_->rects.push_back(global_clip);
    rtree_data_->indices.push_back(op_index_);
  } else {
    save.layer_info->global_space_accumulator.accumulate(global_clip);
  }
  save.layer_info->layer_local_accumulator.accumulate(layer_clip);
  return true;
}

bool DisplayListBuilder::AccumulateOpBounds(SkRect& bounds,
                                            DisplayListAttributeFlags flags) {
  if (AdjustBoundsForPaint(bounds, flags)) {
    return AccumulateBounds(bounds);
  } else {
    return AccumulateUnbounded();
  }
}

bool DisplayListBuilder::AccumulateBounds(const SkRect& bounds,
                                          SaveInfo& layer,
                                          int id) {
  if (bounds.isEmpty()) {
    return false;
  }
  SkRect global_bounds;
  SkRect layer_bounds;
  if (!layer.global_state.mapAndClipRect(bounds, &global_bounds) ||
      !layer.layer_state.mapAndClipRect(bounds, &layer_bounds)) {
    return false;
  }
  if (rtree_data_.has_value()) {
    FML_DCHECK(layer.layer_info->global_space_accumulator.is_empty());
    if (id >= 0) {
      rtree_data_->rects.push_back(global_bounds);
      rtree_data_->indices.push_back(id);
    }
  } else {
    layer.layer_info->global_space_accumulator.accumulate(global_bounds);
  }
  layer.layer_info->layer_local_accumulator.accumulate(layer_bounds);
  return true;
}

bool DisplayListBuilder::SaveInfo::AccumulateBoundsLocal(const SkRect& bounds) {
  if (bounds.isEmpty()) {
    return false;
  }
  SkRect local_bounds;
  if (!layer_state.mapAndClipRect(bounds, &local_bounds)) {
    return false;
  }
  layer_info->layer_local_accumulator.accumulate(local_bounds);
  return true;
}

bool DisplayListBuilder::paint_nops_on_transparency() {
  // SkImageFilter::canComputeFastBounds tests for transparency behavior
  // This test assumes that the blend mode checked down below will
  // NOP on transparent black.
  if (current_.getImageFilterPtr() &&
      current_.getImageFilterPtr()->modifies_transparent_black()) {
    return false;
  }

  // We filter the transparent black that is used for the background of a
  // saveLayer and make sure it returns transparent black. If it does, then
  // the color filter will leave all area surrounding the contents of the
  // save layer untouched out to the edge of the output surface.
  // This test assumes that the blend mode checked down below will
  // NOP on transparent black.
  if (current_.getColorFilterPtr() &&
      current_.getColorFilterPtr()->modifies_transparent_black()) {
    return false;
  }

  // Unusual blendmodes require us to process a saved layer
  // even with operations outside the clip.
  // For example, DstIn is used by masking layers.
  // https://code.google.com/p/skia/issues/detail?id=1291
  // https://crbug.com/401593
  switch (current_.getBlendMode()) {
    // For each of the following transfer modes, if the source
    // alpha is zero (our transparent black), the resulting
    // blended pixel is not necessarily equal to the original
    // destination pixel.
    // Mathematically, any time in the following equations where
    // the result is not d assuming source is 0
    case DlBlendMode::kClear:     // r = 0
    case DlBlendMode::kSrc:       // r = s
    case DlBlendMode::kSrcIn:     // r = s * da
    case DlBlendMode::kDstIn:     // r = d * sa
    case DlBlendMode::kSrcOut:    // r = s * (1-da)
    case DlBlendMode::kDstATop:   // r = d*sa + s*(1-da)
    case DlBlendMode::kModulate:  // r = s*d
      return false;
      break;

    // And in these equations, the result must be d if the
    // source is 0
    case DlBlendMode::kDst:         // r = d
    case DlBlendMode::kSrcOver:     // r = s + (1-sa)*d
    case DlBlendMode::kDstOver:     // r = d + (1-da)*s
    case DlBlendMode::kDstOut:      // r = d * (1-sa)
    case DlBlendMode::kSrcATop:     // r = s*da + d*(1-sa)
    case DlBlendMode::kXor:         // r = s*(1-da) + d*(1-sa)
    case DlBlendMode::kPlus:        // r = min(s + d, 1)
    case DlBlendMode::kScreen:      // r = s + d - s*d
    case DlBlendMode::kOverlay:     // multiply or screen, depending on dest
    case DlBlendMode::kDarken:      // rc = s + d - max(s*da, d*sa),
                                    // ra = kSrcOver
    case DlBlendMode::kLighten:     // rc = s + d - min(s*da, d*sa),
                                    // ra = kSrcOver
    case DlBlendMode::kColorDodge:  // brighten destination to reflect source
    case DlBlendMode::kColorBurn:   // darken destination to reflect source
    case DlBlendMode::kHardLight:   // multiply or screen, depending on source
    case DlBlendMode::kSoftLight:   // lighten or darken, depending on source
    case DlBlendMode::kDifference:  // rc = s + d - 2*(min(s*da, d*sa)),
                                    // ra = kSrcOver
    case DlBlendMode::kExclusion:   // rc = s + d - two(s*d), ra = kSrcOver
    case DlBlendMode::kMultiply:    // r = s*(1-da) + d*(1-sa) + s*d
    case DlBlendMode::kHue:         // ra = kSrcOver
    case DlBlendMode::kSaturation:  // ra = kSrcOver
    case DlBlendMode::kColor:       // ra = kSrcOver
    case DlBlendMode::kLuminosity:  // ra = kSrcOver
      return true;
      break;
  }
}

DlColor DisplayListBuilder::GetEffectiveColor(const DlPaint& paint,
                                              DisplayListAttributeFlags flags) {
  DlColor color;
  if (flags.applies_color()) {
    const DlColorSource* source = paint.getColorSourcePtr();
    if (source) {
      if (source->asColor()) {
        color = source->asColor()->color();
      } else {
        color = source->is_opaque() ? DlColor::kBlack() : kAnyColor;
      }
    } else {
      color = paint.getColor();
    }
  } else if (flags.applies_alpha()) {
    // If the operation applies alpha, but not color, then the only impact
    // of the alpha is to modulate the output towards transparency.
    // We can not guarantee an opaque source even if the alpha is opaque
    // since that would require knowing something about the colors that
    // the alpha is modulating, but we can guarantee a transparent source
    // if the alpha is 0.
    color = (paint.getAlpha() == 0) ? DlColor::kTransparent() : kAnyColor;
  } else {
    color = kAnyColor;
  }
  if (flags.applies_image_filter()) {
    auto filter = paint.getImageFilterPtr();
    if (filter) {
      if (!color.isTransparent() || filter->modifies_transparent_black()) {
        color = kAnyColor;
      }
    }
  }
  if (flags.applies_color_filter()) {
    auto filter = paint.getColorFilterPtr();
    if (filter) {
      if (!color.isTransparent() || filter->modifies_transparent_black()) {
        color = kAnyColor;
      }
    }
  }
  return color;
}

DisplayListBuilder::OpResult DisplayListBuilder::PaintResult(
    const DlPaint& paint,
    DisplayListAttributeFlags flags) {
  if (current_info().is_nop) {
    return OpResult::kNoEffect;
  }
  if (flags.applies_blend()) {
    switch (paint.getBlendMode()) {
      // Nop blend mode (singular, there is only one)
      case DlBlendMode::kDst:
        return OpResult::kNoEffect;

      // Always clears pixels blend mode (singular, there is only one)
      case DlBlendMode::kClear:
        return OpResult::kPreservesTransparency;

      case DlBlendMode::kHue:
      case DlBlendMode::kSaturation:
      case DlBlendMode::kColor:
      case DlBlendMode::kLuminosity:
      case DlBlendMode::kColorBurn:
        return GetEffectiveColor(paint, flags).isTransparent()
                   ? OpResult::kNoEffect
                   : OpResult::kAffectsAll;

      // kSrcIn modifies pixels towards transparency
      case DlBlendMode::kSrcIn:
        return OpResult::kPreservesTransparency;

      // These blend modes preserve destination alpha
      case DlBlendMode::kSrcATop:
      case DlBlendMode::kDstOut:
        return GetEffectiveColor(paint, flags).isTransparent()
                   ? OpResult::kNoEffect
                   : OpResult::kPreservesTransparency;

      // Always destructive blend modes, potentially not affecting transparency
      case DlBlendMode::kSrc:
      case DlBlendMode::kSrcOut:
      case DlBlendMode::kDstATop:
        return GetEffectiveColor(paint, flags).isTransparent()
                   ? OpResult::kPreservesTransparency
                   : OpResult::kAffectsAll;

      // The kDstIn blend mode modifies the destination unless the
      // source color is opaque.
      case DlBlendMode::kDstIn:
        return GetEffectiveColor(paint, flags).isOpaque()
                   ? OpResult::kNoEffect
                   : OpResult::kPreservesTransparency;

      // The next group of blend modes modifies the destination unless the
      // source color is transparent.
      case DlBlendMode::kSrcOver:
      case DlBlendMode::kDstOver:
      case DlBlendMode::kXor:
      case DlBlendMode::kPlus:
      case DlBlendMode::kScreen:
      case DlBlendMode::kMultiply:
      case DlBlendMode::kOverlay:
      case DlBlendMode::kDarken:
      case DlBlendMode::kLighten:
      case DlBlendMode::kColorDodge:
      case DlBlendMode::kHardLight:
      case DlBlendMode::kSoftLight:
      case DlBlendMode::kDifference:
      case DlBlendMode::kExclusion:
        return GetEffectiveColor(paint, flags).isTransparent()
                   ? OpResult::kNoEffect
                   : OpResult::kAffectsAll;

      // Modulate only leaves the pixel alone when the source is white.
      case DlBlendMode::kModulate:
        return GetEffectiveColor(paint, flags) == DlColor::kWhite()
                   ? OpResult::kNoEffect
                   : OpResult::kPreservesTransparency;
    }
  }
  return OpResult::kAffectsAll;
}

}  // namespace flutter
