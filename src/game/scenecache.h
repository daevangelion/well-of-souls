/* The original's JPEG backdrop cache, temp\sceneCache (SceneBackgroundLoad, 0x0048A1A7).
 *
 * With option 30 on, a .jpg backdrop is first resized by the external cresizer.exe into
 * temp\sceneCache\<max(w,h)>_<path with \ / : as _>, and the cached copy is what loads.
 * On a cache miss FUN_0048A05F runs the resizer and blocks the UI thread:
 * Sleep(50), WaitForSingleObject(process, 5000), Sleep(100). The port scales in memory
 * and keeps no cache on disk, so it reproduces only the stall: 150 ms of clock the first
 * time a (size, path) pair is shown in a session. Owner: Core. */
#ifndef WOS_SCENECACHE_H
#define WOS_SCENECACHE_H

/* Call where the original calls SceneBackgroundLoad with client size w x h. */
void scene_cache_load(int w, int h, const char *path);

#endif
