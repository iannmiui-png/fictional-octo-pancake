#!/bin/sh
# The 25 entry points RetroArch resolves. A missing one means the core is
# rejected at load with no useful message, so check rather than assume.
SO=${1:-./aheui_libretro.so}
REQ="retro_init retro_deinit retro_api_version retro_get_system_info
retro_get_system_av_info retro_set_environment retro_set_video_refresh
retro_set_audio_sample retro_set_audio_sample_batch retro_set_input_poll
retro_set_input_state retro_set_controller_port_device retro_reset retro_run
retro_serialize_size retro_serialize retro_unserialize retro_cheat_reset
retro_cheat_set retro_load_game retro_load_game_special retro_unload_game
retro_get_region retro_get_memory_data retro_get_memory_size"
miss=0
for s in $REQ; do
  nm -D --defined-only "$SO" 2>/dev/null | grep -q " T $s\$" || { echo "MISSING $s"; miss=$((miss+1)); }
done
n=$(echo $REQ | wc -w)
echo "entry points: $((n-miss))/$n present"
echo "extra exported symbols:"
nm -D --defined-only "$SO" | awk '$2=="T"{print $3}' | grep -v '^retro_' | sed 's/^/  /' || true
echo "shared library dependencies:"
ldd "$SO" | sed 's/^/  /'
