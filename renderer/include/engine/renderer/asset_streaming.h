// Compatibility shim: the async streaming queue moved to engine::content
// (#171 C1). Deletion condition: the C4 consumer migration removes the
// last include of this renderer-path header.

#pragma once

#include "engine/content/asset_streaming.h"
#include "engine/renderer/asset_metadata.h"

namespace engine::renderer {

using content::LoadPriority;
using content::LoadingState;
using content::LoadHandle;
using content::kInvalidLoadHandle;
using content::AssetLoadCallback;
using content::AssetUploadCallback;
using content::LoadRequest;
using content::AssetStreamingQueue;
using content::initialize_asset_streaming;
using content::shutdown_asset_streaming;
using content::load_asset_async;
using content::update_load_priority;
using content::cancel_load;
using content::release_load;
using content::is_load_ready;
using content::get_load_state;
using content::kWaitForLoadDefaultTimeoutMs;
using content::wait_for_load;
using content::update_asset_streaming;
using content::begin_streaming_frame;
using content::pending_load_count;

} // namespace engine::renderer
