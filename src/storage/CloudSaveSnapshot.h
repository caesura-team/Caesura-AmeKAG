#pragma once

#include "api/ICloudSaveSnapshotTransport.h"

namespace Caesura::detail {

// Storage-internal bounded disk observation; never calls the legacy writer.
CloudSnapshot readLocalCloudSnapshot(const std::string& path);

} // namespace Caesura::detail
