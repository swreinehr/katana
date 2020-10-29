#include "tsuba/FileStorage.h"

#include "FileStorage_internal.h"

std::vector<tsuba::FileStorage*>&
tsuba::GetRegisteredFileStorages() {
  static std::vector<FileStorage*> fs;
  return fs;
}

void
tsuba::RegisterFileStorage(FileStorage* fs) {
  GetRegisteredFileStorages().emplace_back(fs);
}
