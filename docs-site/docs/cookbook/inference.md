# Run TFLite Micro inference on an audio buffer

**Pattern** — fill a model's input tensor with a window of samples, run a TFLite Micro model through `ove_model`, react to the result. The same steps `example_keyword_live` runs on live microphone audio.

**What to enable** (in your `app.yaml`'s `defconfig:` list):

```yaml
defconfig:
  - CONFIG_OVE_CONSOLE=y
  - CONFIG_OVE_LOG=y
  - CONFIG_OVE_INFER=y
  - CONFIG_OVE_THREAD=y
```

You also need a model. With `CONFIG_OVE_INFER` on, the build converts every `.tflite` file under the repository's `models/` directory into a C array: `models/<dir>/my_model.tflite` becomes `g_my_model_model_data.h`, declaring `g_my_model_model_data[]` with its size in `g_my_model_model_data_size`. Every model there is linked into every inference app, so keep only the ones you use.

## Code

```c
#include "ove/ove.h"
#include "ove/log.h"
#include "ove/infer.h"

/* From the generated g_my_model_model_data.h. */
extern const unsigned char g_my_model_model_data[];
#define MODEL_SIZE 18800   /* g_my_model_model_data_size */

#define ARENA_SIZE (32 * 1024)

/* Your audio path: write `len` int8 samples (already scaled). */
void fill_window(int8_t *dst, size_t len);

static uint8_t __attribute__((aligned(16))) arena[ARENA_SIZE];
static ove_model_storage_t model_storage;
static ove_thread_t worker;

static void inference_fn(void *arg)
{
    (void)arg;
    const struct ove_model_config cfg = {
        .model_data = g_my_model_model_data,
        .model_size = MODEL_SIZE,
        .arena_size = ARENA_SIZE,
    };
    ove_model_t model;
    if (ove_model_init(&model, &model_storage, arena, &cfg) != OVE_OK) {
        OVE_LOG_ERR("model load failed (arena too small?)");
        return;
    }

    struct ove_tensor_info in, out;
    ove_model_input(model, 0, &in);
    if (in.type != OVE_TENSOR_INT8) {
        OVE_LOG_ERR("expected an int8 input, got type %d", (int)in.type);
        return;
    }

    while (1) {
        fill_window((int8_t *)in.data, in.size);
        if (ove_model_invoke(model) != OVE_OK) {
            OVE_LOG_ERR("invoke failed");
            continue;
        }
        ove_model_output(model, 0, &out);
        const int8_t *scores = (const int8_t *)out.data;
        size_t best = 0;
        for (size_t i = 1; i < out.size; i++) {
            if (scores[i] > scores[best])
                best = i;
        }
        OVE_LOG_INF("class=%u score=%d (%u us)", (unsigned)best, scores[best],
                    (unsigned)ove_model_last_inference_us(model));
    }
}

void ove_main(void)
{
    if (ove_thread_create(&worker, "infer", inference_fn, NULL, OVE_PRIO_NORMAL, 8192) != OVE_OK)
        return;
    ove_run();
}
```

`ove_model_init()` takes caller storage and works in both heap and zero-heap builds; `ove_model_create()` allocates instead. The input and output pointers stay valid for the life of the model, so read them once.

## Feeding live audio

`fill_window()` is where your samples arrive. For microphone input, build an audio graph (`ove_audio_device_source()` into a processor node) that copies frames into a ring buffer, and let `fill_window()` take the newest window from it. `example_keyword_live` does exactly that, including the feature-extraction model that runs in front of the classifier.

## Tensor arena sizing

The arena holds the model's intermediate tensors. Too small and `ove_model_init()` fails with `OVE_ERR_ML_FAILED`; too large wastes SRAM. There is no call that reports the bytes used, so start generous, shrink until the init fails, and keep a margin above that point. `example_keyword_live` runs both of its models in 32 KB.

## Quantized vs float

`in.type` and `out.type` tell you the element type (`OVE_TENSOR_INT8`, `OVE_TENSOR_INT16`, `OVE_TENSOR_FLOAT32`, ...). Int8 models save flash and run faster on Cortex-M with CMSIS-NN; float32 is easier to author but bigger and slower. `ove_model` does not expose a tensor's scale and zero point: take them from your training pipeline (the TFLite converter reports them per tensor) and apply them when you fill the input and read the scores.

## Where else in the tree

- [API: ML Inference](../api/infer.md) — full surface, lifecycle, quantization handling.
- [`apps/c/heap/example_keyword_live/`](https://github.com/Varcain/oveRTOS/tree/main/apps/c/heap/example_keyword_live) — the canonical "yes" / "no" classifier with DMIC capture and LVGL visualisation.
- [`models/`](https://github.com/Varcain/oveRTOS/tree/main/models) — where models go; the keyword-spotting pair ships there.
