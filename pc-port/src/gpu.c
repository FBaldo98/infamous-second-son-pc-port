// SPDX-License-Identifier: GPL-2.0-or-later
#include "runtime.h"
#include "../gpu/iss_gpu.h"
#include "../gpu/iss_dialog.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uintptr_t (*resolve_import)(const char *);
static int (*handle_fault)(void *,void *);
static int (*readback_memory)(uint64_t,uint64_t);
static int (*invalidate_memory)(uint64_t,uint64_t);
static void (*publish_feedback_identity)(uint64_t,uint64_t,uint64_t);
// A pack of the game opened: its shaders go first in the ahead-of-time compiler (ISS_AOT_COMPUTE).
static void (*file_opened)(const char *);
// The save data dialog drawn by the DLL over the game (gpu/save_dialog_ui.cpp).
static int (*dialog_open)(const IssDialog *);
static int (*dialog_poll)(IssDialogResult *);
static void (*dialog_close)(void);
static int (*dialog_active)(void);
// shadPS4's Windows guest red-zone protection (gpu/red_zone_patches.cpp).
static long long (*patch_red_zone)(unsigned char *,unsigned long long,const unsigned long long *,
                                   const unsigned long long *,unsigned long long,unsigned char *,unsigned long long);
long long gpu_patch_red_zone(unsigned char *image,uint64_t image_size,const uint64_t *addresses,const uint64_t *sizes,
                             uint64_t count,unsigned char *trampoline,uint64_t trampoline_size){
    return patch_red_zone?patch_red_zone(image,image_size,(const unsigned long long *)addresses,(const unsigned long long *)sizes,
                                         count,trampoline,trampoline_size):-1;
}
int gpu_start(uint32_t sdk_version,uint64_t image_base){
    const char *enabled=getenv("ISS_GPU");if(!enabled||strcmp(enabled,"1"))return 0;
    char path[2048];GetModuleFileNameA(NULL,path,sizeof(path));char *slash=strrchr(path,'\\');if(!slash)return -1;strcpy(slash,"\\iss-gpu.dll");
    HMODULE dll=LoadLibraryA(path);if(!dll){fprintf(stderr,"GPU DLL load failed: %lu\n",GetLastError());return -1;}
    int (*initialize)(const IssGpuCallbacks *)=(void *)GetProcAddress(dll,"iss_gpu_initialize");
    resolve_import=(void *)GetProcAddress(dll,"iss_gpu_resolve");handle_fault=(void *)GetProcAddress(dll,"iss_gpu_fault");
    readback_memory=(void *)GetProcAddress(dll,"iss_gpu_readback");
    invalidate_memory=(void *)GetProcAddress(dll,"iss_gpu_invalidate");
    publish_feedback_identity=(void *)GetProcAddress(dll,"iss_gpu_feedback_identity");
    file_opened=(void *)GetProcAddress(dll,"iss_gpu_file_opened");
    void (*mapped)(uint64_t,uint64_t,uint8_t *)=(void *)GetProcAddress(dll,"iss_gpu_map");
    dialog_open=(void *)GetProcAddress(dll,"iss_gpu_dialog_open");dialog_poll=(void *)GetProcAddress(dll,"iss_gpu_dialog_poll");
    dialog_close=(void *)GetProcAddress(dll,"iss_gpu_dialog_close");dialog_active=(void *)GetProcAddress(dll,"iss_gpu_dialog_active");
    patch_red_zone=(void *)GetProcAddress(dll,"iss_gpu_patch_red_zone");
    if(!initialize||!resolve_import||!handle_fault||!mapped)return -1;
    IssGpuCallbacks callbacks={0};callbacks.struct_size=sizeof(callbacks);callbacks.sdk_version=sdk_version;
    callbacks.psf_attributes=545259522;callbacks.width=1280;callbacks.height=720;
    callbacks.serial="CUSA00004";callbacks.title="inFAMOUS Second Son";callbacks.user_dir=runtime_user_root();
    callbacks.tsc_frequency=runtime_tsc_frequency;callbacks.process_time_us=runtime_process_time_us;
    callbacks.errno_location=runtime_errno;callbacks.backing=memory_backing;callbacks.clamp=memory_clamp;
    callbacks.visit_mappings=memory_visit;callbacks.protect=memory_gpu_protect;
    callbacks.image_base=image_base;
    callbacks.mapping_epoch=memory_mapping_epoch;
    callbacks.mapping_lock_shared=memory_mapping_lock_shared;
    callbacks.mapping_unlock_shared=memory_mapping_unlock_shared;
    if(initialize(&callbacks))return -1;memory_set_map_hook(mapped);
    puts("Vulkan renderer initialized; waiting for guest graphics submissions.");return 0;
}
uintptr_t gpu_resolve(const char *name){return resolve_import?resolve_import(name):0;}
int gpu_dialog_available(void){return dialog_open&&dialog_poll&&dialog_close;}
int gpu_dialog_open(const void *dialog){return dialog_open?dialog_open((const IssDialog *)dialog):-1;}
int gpu_dialog_poll(void *result){return dialog_poll?dialog_poll((IssDialogResult *)result):-1;}
void gpu_dialog_close(void){if(dialog_close)dialog_close();}
int gpu_dialog_active(void){return dialog_active?dialog_active():0;}
int gpu_readback(uint64_t address,uint64_t size){return readback_memory?readback_memory(address,size):0;}
int gpu_invalidate(uint64_t address,uint64_t size){return invalidate_memory?invalidate_memory(address,size):0;}
void gpu_file_opened(const char *guest_path){if(file_opened)file_opened(guest_path);}
int gpu_feedback_identity_available(void){return publish_feedback_identity!=NULL;}
void gpu_feedback_identity(uint64_t owner_object,uint64_t shader_object,uint64_t record_address){
    if(publish_feedback_identity)publish_feedback_identity(owner_object,shader_object,record_address);
}
int gpu_handle_fault(EXCEPTION_POINTERS *e){
    if(!handle_fault||e->ExceptionRecord->ExceptionCode!=EXCEPTION_ACCESS_VIOLATION||e->ExceptionRecord->NumberParameters<2)return 0;
    return handle_fault(e,(void *)e->ExceptionRecord->ExceptionInformation[1]);
}
