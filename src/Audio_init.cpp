#include "Audio/AudioInitProps.h"
#include "Common/PlatformDetection.hpp"
#include "Audio_API.h"

extern "C" {
bool  Audio_API Audio_Init(AudioInitProps audioInitProps)
{

	
	AudioAPI_INIT();
	return true;
}
}