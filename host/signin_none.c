/* No sign-in screen (host/signin.c): for hosts whose own UI signs in and passes --user,
 * as the UWP app (FFXIXbox) does. host64 then says so if it was started with neither. No
 * Config > Modern (host/modern.c) either. */
#include "modern.h"
#include "signin.h"

void modern_init(const ModernSetup* setup) { (void)setup; }
void modern_frame(void) {}
void modern_set_addons(const ModernAddons* ops) { (void)ops; }

int signin_run(const SigninSetup* setup, SigninResult* out)
{
    (void)setup, (void)out;
    return -1;
}
