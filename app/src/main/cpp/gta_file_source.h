// Read-only, seekable file-descriptor implementation for user-selected GTA
// data. Android's Storage Access Framework owns permission/UI; this class only
// duplicates an already-authorized descriptor and serves bounded pread calls.
#pragma once

#include "gta_img_archive.h"

#include <memory>
#include <string>

namespace samp::gta::img {

class FileDescriptorByteSource final : public ByteSource {
public:
  // Duplicates `fileDescriptor` and takes ownership of the duplicate. The
  // caller keeps ownership of its original descriptor/PFD and may close it
  // after this call returns. Only regular, seekable files are accepted.
  static std::unique_ptr<FileDescriptorByteSource> duplicateFrom(int fileDescriptor,
                                                                   std::string& error);

  ~FileDescriptorByteSource() override;
  FileDescriptorByteSource(const FileDescriptorByteSource&) = delete;
  FileDescriptorByteSource& operator=(const FileDescriptorByteSource&) = delete;

  uint64_t size() const override;
  bool readAt(uint64_t offset, uint8_t* destination, size_t count) const override;

private:
  FileDescriptorByteSource(int ownedFileDescriptor, uint64_t fileSize);

  int fileDescriptor_ = -1;
  uint64_t fileSize_ = 0;
};

}  // namespace samp::gta::img
