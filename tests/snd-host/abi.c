/* Compare our private kernel ABI with independent Linux UAPI definitions. */
#include <stddef.h>
#include <sys/ioctl.h>
#include <sound/asound.h>
#include <kernel/pcm_abi.h>
#define SIZE(a,b) _Static_assert(sizeof(struct a)==sizeof(struct b), #a " size")
#define OFF(a,b,f) _Static_assert(offsetof(struct a,f)==offsetof(struct b,f), #a "." #f)
SIZE(pcm_info,snd_pcm_info); SIZE(pcm_hw_params,snd_pcm_hw_params);
SIZE(pcm_sw_params,snd_pcm_sw_params); SIZE(pcm_status,snd_pcm_status);
SIZE(pcm_sync_ptr,snd_pcm_sync_ptr); SIZE(pcm_xferi,snd_xferi);
OFF(pcm_hw_params,snd_pcm_hw_params,masks); OFF(pcm_hw_params,snd_pcm_hw_params,intervals);
OFF(pcm_hw_params,snd_pcm_hw_params,rmask); OFF(pcm_hw_params,snd_pcm_hw_params,fifo_size);
OFF(pcm_sw_params,snd_pcm_sw_params,avail_min); OFF(pcm_sw_params,snd_pcm_sw_params,boundary);
OFF(pcm_status,snd_pcm_status,appl_ptr); OFF(pcm_status,snd_pcm_status,hw_ptr);
OFF(pcm_status,snd_pcm_status,delay); OFF(pcm_status,snd_pcm_status,audio_tstamp);
OFF(pcm_sync_ptr,snd_pcm_sync_ptr,s); OFF(pcm_sync_ptr,snd_pcm_sync_ptr,c);
_Static_assert(PCM_IO_HW_REFINE==SNDRV_PCM_IOCTL_HW_REFINE,"refine ioctl");
_Static_assert(PCM_IO_HW_PARAMS==SNDRV_PCM_IOCTL_HW_PARAMS,"params ioctl");
_Static_assert(PCM_IO_SW_PARAMS==SNDRV_PCM_IOCTL_SW_PARAMS,"sw ioctl");
_Static_assert(PCM_IO_STATUS==SNDRV_PCM_IOCTL_STATUS,"status ioctl");
_Static_assert(PCM_IO_SYNC_PTR==SNDRV_PCM_IOCTL_SYNC_PTR,"sync ioctl");
_Static_assert(PCM_IO_WRITEI==SNDRV_PCM_IOCTL_WRITEI_FRAMES,"write ioctl");
int main(void) { return 0; }
