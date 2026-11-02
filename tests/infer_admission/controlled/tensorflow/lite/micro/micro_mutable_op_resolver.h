// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "tensorflow/lite/common.hpp"
namespace tflite
{
template <int N> struct MicroMutableOpResolver : MicroOpResolver {
	MicroMutableOpResolver()
	{
		infer_probe_resolver(1);
	}
	~MicroMutableOpResolver()
	{
		infer_probe_resolver(-1);
	}
	TfLiteStatus AddAdd()
	{
		return static_cast<TfLiteStatus>(infer_probe_register());
	}
	TfLiteStatus AddMul()
	{
		return static_cast<TfLiteStatus>(infer_probe_register());
	}
	TfLiteStatus AddSub()
	{
		return static_cast<TfLiteStatus>(infer_probe_register());
	}
	TfLiteStatus AddDiv()
	{
		return static_cast<TfLiteStatus>(infer_probe_register());
	}
	TfLiteStatus AddConv2D()
	{
		return static_cast<TfLiteStatus>(infer_probe_register());
	}
	TfLiteStatus AddDepthwiseConv2D()
	{
		return static_cast<TfLiteStatus>(infer_probe_register());
	}
	TfLiteStatus AddTransposeConv()
	{
		return static_cast<TfLiteStatus>(infer_probe_register());
	}
	TfLiteStatus AddRelu()
	{
		return static_cast<TfLiteStatus>(infer_probe_register());
	}
	TfLiteStatus AddRelu6()
	{
		return static_cast<TfLiteStatus>(infer_probe_register());
	}
	TfLiteStatus AddTanh()
	{
		return static_cast<TfLiteStatus>(infer_probe_register());
	}
	TfLiteStatus AddLogistic()
	{
		return static_cast<TfLiteStatus>(infer_probe_register());
	}
	TfLiteStatus AddSoftmax()
	{
		return static_cast<TfLiteStatus>(infer_probe_register());
	}
	TfLiteStatus AddMaxPool2D()
	{
		return static_cast<TfLiteStatus>(infer_probe_register());
	}
	TfLiteStatus AddAveragePool2D()
	{
		return static_cast<TfLiteStatus>(infer_probe_register());
	}
	TfLiteStatus AddFullyConnected()
	{
		return static_cast<TfLiteStatus>(infer_probe_register());
	}
	TfLiteStatus AddReshape()
	{
		return static_cast<TfLiteStatus>(infer_probe_register());
	}
	TfLiteStatus AddConcatenation()
	{
		return static_cast<TfLiteStatus>(infer_probe_register());
	}
	TfLiteStatus AddPad()
	{
		return static_cast<TfLiteStatus>(infer_probe_register());
	}
	TfLiteStatus AddPadV2()
	{
		return static_cast<TfLiteStatus>(infer_probe_register());
	}
	TfLiteStatus AddSplit()
	{
		return static_cast<TfLiteStatus>(infer_probe_register());
	}
	TfLiteStatus AddSplitV()
	{
		return static_cast<TfLiteStatus>(infer_probe_register());
	}
	TfLiteStatus AddStridedSlice()
	{
		return static_cast<TfLiteStatus>(infer_probe_register());
	}
	TfLiteStatus AddSqueeze()
	{
		return static_cast<TfLiteStatus>(infer_probe_register());
	}
	TfLiteStatus AddExpandDims()
	{
		return static_cast<TfLiteStatus>(infer_probe_register());
	}
	TfLiteStatus AddTranspose()
	{
		return static_cast<TfLiteStatus>(infer_probe_register());
	}
	TfLiteStatus AddQuantize()
	{
		return static_cast<TfLiteStatus>(infer_probe_register());
	}
	TfLiteStatus AddDequantize()
	{
		return static_cast<TfLiteStatus>(infer_probe_register());
	}
	TfLiteStatus AddMean()
	{
		return static_cast<TfLiteStatus>(infer_probe_register());
	}
	TfLiteStatus AddMaximum()
	{
		return static_cast<TfLiteStatus>(infer_probe_register());
	}
	TfLiteStatus AddMinimum()
	{
		return static_cast<TfLiteStatus>(infer_probe_register());
	}
	TfLiteStatus AddNeg()
	{
		return static_cast<TfLiteStatus>(infer_probe_register());
	}
	TfLiteStatus AddRsqrt()
	{
		return static_cast<TfLiteStatus>(infer_probe_register());
	}
	TfLiteStatus AddCast()
	{
		return static_cast<TfLiteStatus>(infer_probe_register());
	}
	TfLiteStatus AddResizeBilinear()
	{
		return static_cast<TfLiteStatus>(infer_probe_register());
	}
	TfLiteStatus AddResizeNearestNeighbor()
	{
		return static_cast<TfLiteStatus>(infer_probe_register());
	}
	TfLiteStatus AddBatchMatMul()
	{
		return static_cast<TfLiteStatus>(infer_probe_register());
	}
	TfLiteStatus AddWindow()
	{
		return static_cast<TfLiteStatus>(infer_probe_register());
	}
	TfLiteStatus AddFftAutoScale()
	{
		return static_cast<TfLiteStatus>(infer_probe_register());
	}
	TfLiteStatus AddRfft()
	{
		return static_cast<TfLiteStatus>(infer_probe_register());
	}
	TfLiteStatus AddEnergy()
	{
		return static_cast<TfLiteStatus>(infer_probe_register());
	}
	TfLiteStatus AddFilterBank()
	{
		return static_cast<TfLiteStatus>(infer_probe_register());
	}
	TfLiteStatus AddFilterBankSquareRoot()
	{
		return static_cast<TfLiteStatus>(infer_probe_register());
	}
	TfLiteStatus AddFilterBankSpectralSubtraction()
	{
		return static_cast<TfLiteStatus>(infer_probe_register());
	}
	TfLiteStatus AddPCAN()
	{
		return static_cast<TfLiteStatus>(infer_probe_register());
	}
	TfLiteStatus AddFilterBankLog()
	{
		return static_cast<TfLiteStatus>(infer_probe_register());
	}
};
} // namespace tflite
