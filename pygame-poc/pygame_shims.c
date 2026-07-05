/* wasthon pygame port — small C shims for symbols pygame-ce references but
 * that aren't provided by this build (kept out of the bridge proper). */
#include <SDL.h>

/* SDL2_gfx's SDL_rotozoom.c is not bundled in this pygame-ce clone's
 * src_c/SDL_gfx/ (only SDL_gfxPrimitives.c is). transform.rotozoom pulls
 * rotozoomSurface; stub it (returns NULL = unsupported) so the module links.
 * TODO: vendor SDL_rotozoom.c to make pygame.transform.rotozoom work. */
SDL_Surface *
rotozoomSurface(SDL_Surface *src, double angle, double zoom, int smooth)
{
    (void)src; (void)angle; (void)zoom; (void)smooth;
    return NULL;
}
