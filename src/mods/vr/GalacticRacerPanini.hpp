#pragma once

#include "GalacticRacerPaniniPolicy.hpp"

namespace uevr::swgr_panini {
void update(bool cvar_option, bool component_option, bool hmd_active);
void shutdown();
Status cvar_status();
Status component_status();
const char* status_text(Status status);
}
