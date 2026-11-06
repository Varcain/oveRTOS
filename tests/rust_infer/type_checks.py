# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compiler-only ownership contracts; no model or consumer program executes."""

import hashlib
import json
import os
import subprocess
import sys
from pathlib import Path

PREFIX = "#![allow(dead_code, unused_variables, unused_mut, unused_unsafe, unused_imports)]\nuse ove::infer::{Model, ModelConfig, ModelStorage};\n#[repr(C, align(16))] struct Aligned([u8;64]);\nfn config(data: &[u8]) -> ModelConfig<'_> { unsafe { ModelConfig::from_trusted_model(data, 128).unwrap() } }\nfn consume<T>(value:T) { std::hint::black_box(value); }\n"
CASES = {
    "positive_borrowed": (
        "let data = Aligned([0;64]); let mut storage = ModelStorage::<128>::new(); let mut model = storage.load(&config(&data.0)).unwrap();\n{ let v = model.input_slice_mut::<i16>(0).unwrap(); consume(v.len()); } unsafe { model.invoke().unwrap(); } { let v=model.output_slice::<i8>(0).unwrap(); consume(v.len()); } drop(model); let again=storage.load(&config(&data.0)).unwrap(); drop(again);",
        None,
    ),
    "positive_send_sync": (
        "let data = Aligned([0;64]); let mut storage = ModelStorage::<128>::new(); let mut model = storage.load(&config(&data.0)).unwrap();\nfn needs_send<T:Send>(_:&T){} fn needs_sync<T:Sync>(_:&T){} needs_send(&model); needs_sync(&model); consume(model.input(0)); consume(model.last_inference_us());",
        None,
    ),
    "positive_all_scalars": (
        "let data = Aligned([0;64]); let mut storage = ModelStorage::<128>::new(); let mut model = storage.load(&config(&data.0)).unwrap();\nconsume(model.input_slice_mut::<f32>(0)); consume(model.input_slice_mut::<i8>(0)); consume(model.input_slice_mut::<u8>(0)); consume(model.input_slice_mut::<i16>(0)); consume(model.input_slice_mut::<i32>(0));",
        None,
    ),
    "lost_model_bytes": (
        "let mut storage=ModelStorage::<128>::new(); let model; { let data=Aligned([0;64]); model=storage.load(&config(&data.0)).unwrap(); } consume(model.arena_used());",
        "E0597",
    ),
    "lost_storage": (
        "let data=Aligned([0;64]); let model; { let mut storage=ModelStorage::<128>::new(); model=storage.load(&config(&data.0)).unwrap(); } consume(model.arena_used());",
        "E0597",
    ),
    "storage_move_with_owner": (
        "let data = Aligned([0;64]); let mut storage = ModelStorage::<128>::new(); let mut model = storage.load(&config(&data.0)).unwrap();\nconsume(storage); consume(model.arena_used());",
        "E0505",
    ),
    "storage_overwrite_with_owner": (
        "let data = Aligned([0;64]); let mut storage = ModelStorage::<128>::new(); let mut model = storage.load(&config(&data.0)).unwrap();\nstorage=ModelStorage::new(); consume(model.arena_used());",
        "E0506",
    ),
    "duplicate_mutable_input": (
        "let data = Aligned([0;64]); let mut storage = ModelStorage::<128>::new(); let mut model = storage.load(&config(&data.0)).unwrap();\nlet a=model.input_slice_mut::<i16>(0).unwrap(); let b=model.input_slice_mut::<i16>(0).unwrap(); a[0]=1; b[0]=2;",
        "E0499",
    ),
    "different_input_indices": (
        "let data = Aligned([0;64]); let mut storage = ModelStorage::<128>::new(); let mut model = storage.load(&config(&data.0)).unwrap();\nlet a=model.input_slice_mut::<i16>(0).unwrap(); let b=model.input_slice_mut::<i16>(1).unwrap(); a[0]=1; b[0]=2;",
        "E0499",
    ),
    "output_live_invoke": (
        "let data = Aligned([0;64]); let mut storage = ModelStorage::<128>::new(); let mut model = storage.load(&config(&data.0)).unwrap();\nlet out=model.output_slice::<i8>(0).unwrap(); unsafe { model.invoke().unwrap(); } consume(out);",
        "E0502",
    ),
    "output_live_input_write": (
        "let data = Aligned([0;64]); let mut storage = ModelStorage::<128>::new(); let mut model = storage.load(&config(&data.0)).unwrap();\nlet out=model.output_slice::<i8>(0).unwrap(); let input=model.input_slice_mut::<i8>(0).unwrap(); consume(out); consume(input);",
        "E0502",
    ),
    "output_live_storage_move": (
        "let data = Aligned([0;64]); let mut storage = ModelStorage::<128>::new(); let mut model = storage.load(&config(&data.0)).unwrap();\nlet out=model.output_slice::<i8>(0).unwrap(); consume(storage); consume(out);",
        "E0505",
    ),
    "unsupported_bool": (
        "let data = Aligned([0;64]); let mut storage = ModelStorage::<128>::new(); let mut model = storage.load(&config(&data.0)).unwrap();\nconsume(model.input_slice_mut::<bool>(0));",
        "E0277",
    ),
    "unsupported_reference": (
        "let data = Aligned([0;64]); let mut storage = ModelStorage::<128>::new(); let mut model = storage.load(&config(&data.0)).unwrap();\nconsume(model.output_slice::<&u8>(0));",
        "E0277",
    ),
    "unsupported_zero_size": (
        "let data = Aligned([0;64]); let mut storage = ModelStorage::<128>::new(); let mut model = storage.load(&config(&data.0)).unwrap();\nconsume(model.input_slice_mut::<()>(0));",
        "E0277",
    ),
    "unsupported_enum": (
        "let data = Aligned([0;64]); let mut storage = ModelStorage::<128>::new(); let mut model = storage.load(&config(&data.0)).unwrap();\nenum Bad { A } consume(model.output_slice::<Bad>(0));",
        "E0277",
    ),
    "unsupported_alignment": (
        "let data = Aligned([0;64]); let mut storage = ModelStorage::<128>::new(); let mut model = storage.load(&config(&data.0)).unwrap();\n#[repr(align(64))] struct OverAligned(u8); consume(model.output_slice::<OverAligned>(0));",
        "E0277",
    ),
    "shared_invoke": (
        "let data = Aligned([0;64]); let mut storage = ModelStorage::<128>::new(); let mut model = storage.load(&config(&data.0)).unwrap();\nlet shared=&model; unsafe { shared.invoke().unwrap(); }",
        "E0596",
    ),
    "shared_input_write": (
        "let data = Aligned([0;64]); let mut storage = ModelStorage::<128>::new(); let mut model = storage.load(&config(&data.0)).unwrap();\nlet shared=&model; consume(shared.input_slice_mut::<i8>(0));",
        "E0596",
    ),
    "safe_unchecked_invoke": (
        "let data = Aligned([0;64]); let mut storage = ModelStorage::<128>::new(); let mut model = storage.load(&config(&data.0)).unwrap();\nmodel.invoke().unwrap();",
        "E0133",
    ),
    "safe_linked_asset": (
        "ove::model_data!(linked_bytes, fixture_model_bytes, fixture_model_len); consume(linked_bytes());",
        "E0133",
    ),
    "mutable_qualified_capacity": (
        "let data=Aligned([0;64]); let mut cfg=config(&data.0); cfg.arena_size=1; consume(cfg);",
        "E0616",
    ),
    "direct_model_drop_already_rejected": (
        "let data = Aligned([0;64]); let mut storage = ModelStorage::<128>::new(); let mut model = storage.load(&config(&data.0)).unwrap();\nlet out=model.output_slice::<i8>(0).unwrap(); drop(model); consume(out);",
        "E0505",
    ),
    "positive_ephemeral_heap_config": (
        "let data=Aligned([0;64]); let model=Model::new(&config(&data.0)).unwrap(); consume(model.arena_used());",
        None,
    ),
    "lost_heap_model_bytes": (
        "let model; { let data=Aligned([0;64]); model=Model::new(&config(&data.0)).unwrap(); } consume(model.arena_used());",
        "E0597",
    ),
    "drop_heap_byte_allocation": (
        "let data=Box::new(Aligned([0;64])); let model=Model::new(&config(&data.0)).unwrap(); drop(data); consume(model.arena_used());",
        "E0505",
    ),
    "mutate_heap_model_bytes": (
        "let mut data=Box::new(Aligned([0;64])); let model=Model::new(&config(&data.0)).unwrap(); data.0[0]=2; consume(model.arena_used());",
        "E0506",
    ),
    "scalar_is_sealed": (
        "#[derive(Clone, Copy)] struct Custom; impl ove::infer::TensorElement for Custom { const TYPE: ove::infer::TensorType = ove::infer::TensorType::Int32; }",
        "E0277",
    ),
    "qualification_requires_unsafe": (
        "let data=Aligned([0;64]); consume(ModelConfig::from_trusted_model(&data.0,128));",
        "E0133",
    ),
}

CALLBACK = """
use std::sync::Arc;
use std::task::{Wake, Waker, Context};
use std::future::Future;
use ove::infer::Model;
use ove::sync::SpinMutex;
use ove::async_runtime::AsyncInput;
use ove::gpio::{GpioPin, GpioIrqMode};
struct DropOnWake { owner: SpinMutex<Option<Model<'static, 'static>>> }
impl Wake for DropOnWake {
    fn wake(self: Arc<Self>) { self.wake_by_ref(); }
    fn wake_by_ref(self: &Arc<Self>) { drop(self.owner.with(Option::take)); }
}
static INPUT: AsyncInput = AsyncInput::new(GpioPin::new(0, 0));
pub fn compile_safe_callback_route(model: Model<'static, 'static>) {
    let thread_owner = Arc::new(DropOnWake { owner: SpinMutex::new(Some(model)) });
    let waker = Waker::from(thread_owner.clone());
    let mut context = Context::from_waker(&waker);
    let mut wait = Box::pin(INPUT.wait_for_event());
    INPUT.arm(GpioIrqMode::Both).unwrap();
    let _ = wait.as_mut().poll(&mut context);
    drop(thread_owner);
}
"""


def callback_contract(root, work):
    work = work / "callback"
    (work / "src").mkdir(parents=True, exist_ok=True)
    (work / "Cargo.toml").write_text(
        '[package]\nname="ove-infer-callback-check"\nversion="0.0.0"\nedition="2024"\n'
        "[workspace]\n[dependencies]\nove={path="
        + json.dumps(str(root / "bindings/rust/ove"))
        + ',default-features=false,features=["std","async"]}\n'
    )
    if not (work / "Cargo.lock").exists():
        (work / "Cargo.lock").write_bytes(
            (root / "tests/rust_async_spi/Cargo.lock").read_bytes()
        )
    (work / "src/lib.rs").write_text(CALLBACK)
    outcome = subprocess.run(
        [
            "cargo",
            "check",
            "--offline",
            "--manifest-path",
            str(work / "Cargo.toml"),
            "--lib",
        ],
        capture_output=True,
        text=True,
        timeout=120,
        check=False,
    )
    (work / "check.log").write_text(outcome.stdout + outcome.stderr)
    assert outcome.returncode == 0, outcome.stderr


def main():
    root = Path(os.environ["OVE_DIR"])
    work = Path(sys.argv[1]).resolve()
    zero = sys.argv[2] == "zero"
    (work / "src").mkdir(parents=True, exist_ok=True)
    (work / "Cargo.toml").write_text(
        '[package]\nname="ove-infer-type-checks"\nversion="0.0.0"\nedition="2024"\n'
        "[workspace]\n[dependencies]\nove={path="
        + json.dumps(str(root / "bindings/rust/ove"))
        + ',default-features=false,features=["std"]}\n'
    )
    if not (work / "Cargo.lock").exists():
        (work / "Cargo.lock").write_bytes(
            (root / "tests/rust_infer/Cargo.lock").read_bytes()
        )
    results = []
    for name, (body, expected) in CASES.items():
        if zero and "heap" in name:
            continue
        code = PREFIX + "pub fn compile_contract() {\n" + body + "\n}\n"
        (work / "src/lib.rs").write_text(code)
        outcome = subprocess.run(
            [
                "cargo",
                "check",
                "--offline",
                "--manifest-path",
                str(work / "Cargo.toml"),
                "--lib",
            ],
            capture_output=True,
            text=True,
            timeout=120,
            check=False,
        )
        text = outcome.stdout + outcome.stderr
        (work / (name + ".log")).write_text(text)
        if expected is None:
            assert outcome.returncode == 0, (name, text)
        else:
            assert outcome.returncode != 0 and f"error[{expected}]" in text, (
                name,
                text,
            )
        results.append(
            {
                "case": name,
                "diagnostic": expected,
                "source_sha256": hashlib.sha256(code.encode()).hexdigest(),
            }
        )
    (work / "results.json").write_text(json.dumps(results, indent=2) + "\n")
    callback_contract(root, work)
    print(f"Inference external type contracts: {len(results)} passed")
    print("Safe callback ownership route: compiler-only positive control passed")


if __name__ == "__main__":
    main()
