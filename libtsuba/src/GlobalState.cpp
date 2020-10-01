#include "GlobalState.h"

#include <algorithm>
#include <cassert>

#include "galois/Logging.h"
#include "galois/Result.h"
#include "tsuba/NameServerClient.h"

namespace tsuba {

extern GlobalFileStorageAllocator azure_storage_allocator;
extern GlobalFileStorageAllocator local_storage_allocator;
extern GlobalFileStorageAllocator s3_storage_allocator;

}  // namespace tsuba

namespace {

std::vector<tsuba::GlobalFileStorageAllocator*> available_storage_allocators{
#ifdef GALOIS_HAVE_AZURE_BACKEND
    &tsuba::azure_storage_allocator,
#endif
#ifdef GALOIS_HAVE_S3_BACKEND
    &tsuba::s3_storage_allocator,
#endif
#ifdef GALOIS_HAVE_LOCAL_BACKEND
    &tsuba::local_storage_allocator,
#endif
};

}  // namespace

namespace tsuba {

std::unique_ptr<tsuba::GlobalState> GlobalState::ref_ = nullptr;

galois::CommBackend*
GlobalState::Comm() const {
  assert(comm_ != nullptr);
  return comm_;
}

FileStorage*
GlobalState::GetDefaultFS() const {
  assert(file_stores_.size() > 0);
  return file_stores_[0].get();
}

tsuba::FileStorage*
GlobalState::FS(std::string_view uri) const {
  for (const std::unique_ptr<FileStorage>& fs : file_stores_) {
    if (uri.find(fs->uri_scheme()) == 0) {
      return fs.get();
    }
  }
  return GetDefaultFS();
}

NameServerClient*
GlobalState::NS() const {
  return name_server_client_;
}

galois::Result<void>
GlobalState::Init(galois::CommBackend* comm, tsuba::NameServerClient* ns) {
  assert(ref_ == nullptr);

  // quick ping to say hello and fail fast if something was misconfigured
  if (auto res = ns->CheckHealth(); !res) {
    return res.error();
  }

  // new to access non-public constructor
  std::unique_ptr<GlobalState> global_state(new GlobalState(comm, ns));

  for (GlobalFileStorageAllocator* allocator : available_storage_allocators) {
    global_state->file_stores_.emplace_back(allocator->allocate());
  }

  std::sort(
      global_state->file_stores_.begin(), global_state->file_stores_.end(),
      [](const std::unique_ptr<FileStorage>& lhs,
         const std::unique_ptr<FileStorage>& rhs) {
        return lhs->Priority() > rhs->Priority();
      });

  for (std::unique_ptr<FileStorage>& storage : global_state->file_stores_) {
    if (auto res = storage->Init(); !res) {
      return res.error();
    }
  }

  ref_ = std::move(global_state);
  return galois::ResultSuccess();
}

galois::Result<void>
GlobalState::Fini() {
  for (std::unique_ptr<FileStorage>& fs : ref_->file_stores_) {
    if (auto res = fs->Fini(); !res) {
      return res.error();
    }
  }
  ref_.reset(nullptr);
  return galois::ResultSuccess();
}

const tsuba::GlobalState&
GlobalState::Get() {
  assert(ref_ != nullptr);
  return *ref_;
}

}  // namespace tsuba

galois::CommBackend*
tsuba::Comm() {
  return GlobalState::Get().Comm();
}

tsuba::FileStorage*
tsuba::FS(std::string_view uri) {
  return GlobalState::Get().FS(uri);
}

tsuba::NameServerClient*
tsuba::NS() {
  return GlobalState::Get().NS();
}

galois::Result<void>
tsuba::OneHostOnly(const std::function<galois::Result<void>()>& cb) {
  bool failed = false;
  if (Comm()->ID == 0) {
    auto res = cb();
    if (!res) {
      GALOIS_LOG_ERROR("OneHostOnly operation failed: {}", res.error());
      failed = true;
    }
  }
  if (Comm()->Broadcast(0, failed)) {
    return ErrorCode::MpiError;
  }
  return galois::ResultSuccess();
}
