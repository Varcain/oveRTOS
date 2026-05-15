# Run TFLite Micro inference on an audio buffer

**Pattern** — collect a fixed-length audio window from an audio source, feed it to a TFLite Micro model via `ove_infer`, react to the result. Mirrors the `example_keyword_live` app at smaller scale.

**What to enable** (in your `app.yaml`'s `defconfig:` list):

```yaml
defconfig:
  - CONFIG_OVE_CONSOLE=y
  - CONFIG_OVE_LOG=y
  - CONFIG_OVE_INFER=y
  - CONFIG_OVE_AUDIO=y
  - CONFIG_OVE_QUEUE=y
  - CONFIG_OVE_THREAD=y
```

You also need a model. The build system can convert a `.tflite` file into a C array at compile time — drop the file under `models/` and add this to your app's `app.yaml`:

```yaml
models:
  - path: models/your_model.tflite
    sym:  YOUR_MODEL
```

The build emits `generated/your_model_model_data.h` exposing `YOUR_MODEL_data[]` and `YOUR_MODEL_data_len`.

## Code

```c
#include "ove/ove.h"
#include "ove/log.h"
#include "ove/infer.h"
#include "ove/audio.h"
#include "generated/your_model_model_data.h"

OVE_LOG_MODULE_REGISTER(inf);

#define WINDOW_SAMPLES  16000    /* 1 s @ 16 kHz */
#define TENSOR_ARENA    64 * 1024

static uint8_t tensor_arena[TENSOR_ARENA] __attribute__((aligned(16)));
static ove_infer_t model;

static int16_t  window[WINDOW_SAMPLES];
static size_t   filled;

/* Pull a window's worth of audio off the queue, then run inference. */
static void inference_thread(void *arg)
{
    (void)arg;
    while (1) {
        int16_t batch[256];
        size_t n = audio_source_pull(batch, sizeof(batch) / sizeof(batch[0]));

        size_t to_copy = n < (WINDOW_SAMPLES - filled)
                       ? n : (WINDOW_SAMPLES - filled);
        memcpy(&window[filled], batch, to_copy * sizeof(int16_t));
        filled += to_copy;

        if (filled < WINDOW_SAMPLES) continue;

        /* Run model.  Input tensor is int16 PCM. */
        int16_t *in = ove_infer_input_data(model, 0);
        memcpy(in, window, sizeof(window));

        if (ove_infer_invoke(model) != OVE_OK) {
            OVE_LOG_ERR("invoke failed");
            filled = 0;
            continue;
        }

        const float *out = ove_infer_output_data(model, 0);
        size_t classes  = ove_infer_output_count(model, 0);

        int best = 0;
        float best_p = out[0];
        for (size_t i = 1; i < classes; i++) {
            if (out[i] > best_p) { best_p = out[i]; best = (int)i; }
        }
        OVE_LOG_INF("class=%d p=%.2f", best, (double)best_p);

        filled = 0;   /* next window */
    }
}

void ove_main(void)
{
    if (ove_infer_create(&model, YOUR_MODEL_data, YOUR_MODEL_data_len,
                         tensor_arena, sizeof(tensor_arena)) != OVE_OK) {
        OVE_LOG_ERR("model load failed");
        return;
    }

    /* Open an audio source (e.g., DMIC) — see ove_audio_device docs. */
    audio_source_open(16000 /* Hz */, 1 /* mono */);

    static ove_thread_t inf_thread;
    ove_thread_create(&inf_thread, "infer", inference_thread, NULL,
                      OVE_PRIO_NORMAL, 8192);

    ove_run();
}
```

## Tensor arena sizing

Too small and `ove_infer_invoke` fails. Too large and you waste SRAM. The conventional approach:

1. Start with `64 * 1024` (64 KB).
2. After `ove_infer_create` succeeds, call `ove_infer_arena_used(model)` to read the peak used bytes.
3. Tighten the arena to peak + 10 %.

The keyword-live model fits in 16 KB; image classification models often need 256 KB+.

## Quantized vs float

`ove_infer_input_dtype(model, 0)` tells you what to write. Int8/int16 quantized models save flash and run faster on Cortex-M with CMSIS-NN; float32 is easier to author but bigger and slower.

For int8 inputs:

```c
int8_t *in = ove_infer_input_data(model, 0);
float scale; int32_t zero_point;
ove_infer_input_quant_params(model, 0, &scale, &zero_point);
for (size_t i = 0; i < WINDOW_SAMPLES; i++) {
    in[i] = (int8_t)(window[i] / 256 + zero_point);   /* approximate */
}
```

The exact quantization params come from your trained model.

## Where else in the tree

- [API: ML Inference](../api/infer.md) — full surface, lifecycle, quantization handling.
- [`apps/c/heap/example_keyword_live/`](https://github.com/Varcain/oveRTOS/tree/main/apps/c/heap/example_keyword_live) — the canonical "yes" / "no" classifier with DMIC capture and LVGL visualisation.
- [`models/`](https://github.com/Varcain/oveRTOS/tree/main/models) — bundled models you can target without retraining.
