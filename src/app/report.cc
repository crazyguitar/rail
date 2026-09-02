#include "rail/app/report.h"

#include <sstream>

namespace rail {

std::string Report::toJson() const {
  std::ostringstream O;
  O << "{\"files\":" << Files << ",\"file_size\":" << FileSize << ",\"literal_bytes\":" << LiteralBytes << ",\"matched_bytes\":" << MatchedBytes
    << ",\"hash_hits\":" << HashHits << ",\"false_alarms\":" << FalseAlarms << ",\"scan_ns\":" << ScanTime.count()
    << ",\"transfer_ns\":" << TransferTime.count() << ",\"backend\":\"" << Backend << "\",\"rails\":\"" << Rails
    << "\",\"delta_used\":" << (DeltaUsed ? "true" : "false") << "}";
  return O.str();
}

} // namespace rail
