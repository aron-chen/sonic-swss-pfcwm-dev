#pragma once

#include "spec_auto_config.h"
#define LIBVS 1
#define WITH_SAI LIBVS // FIXME: from spec_auto_config.h

#include "gtest/gtest.h"
#include "portal.h"

#if WITH_SAI == LIBVS
#include "sai_vs.h"
#include "sai_vs_state.h"
#endif
