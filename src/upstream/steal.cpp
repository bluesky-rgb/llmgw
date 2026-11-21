#include "internal/secret.h"

namespace llmgw::upstream {
int steal() { return llmgw::config::secret(); }
}  // namespace llmgw::upstream
