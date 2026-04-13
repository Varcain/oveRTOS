/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/**
 * @file lvgl.hpp
 * @brief LVGL display integration — guards, widgets, styles, and reactive state
 */

#pragma once

#include <ove/lvgl.h>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <type_traits>
#include <concepts>

/**
 * @namespace ove::lvgl
 * @brief C++ abstractions for the LVGL embedded GUI library.
 *
 * This namespace provides:
 * - `LvglGuard` — RAII lock for thread-safe LVGL access.
 * - `ObjectView` — lightweight, non-owning wrapper around `lv_obj_t*`.
 * - CRTP mixin templates (`ObjectMixin`, `EventMixin`, `StyleMixin`) that
 *   add fluent setter chains to concrete widget types.
 * - `Style` — RAII wrapper for `lv_style_t`.
 * - `State<T>` — reactive integer state backed by the LVGL observer
 *   (requires `LV_USE_OBSERVER`).
 * - Concrete widget wrappers: `Label`, `Bar`, `Box`.
 * - Layout helper functions: `vbox()`, `hbox()`.
 * - `Component<Derived>` — CRTP base for reusable UI components.
 *
 * @note All methods that modify LVGL objects must be called from the LVGL
 *       task context or while holding the `LvglGuard` lock.
 */
namespace ove::lvgl {

/* ================================================================== */
/*  LvglGuard — RAII lock for thread-safe LVGL access                 */
/* ================================================================== */

/**
 * @class LvglGuard
 * @brief RAII scoped lock for thread-safe access to the LVGL rendering context.
 *
 * Acquires the LVGL lock on construction (via `ove_lvgl_lock()`) and
 * releases it on destruction (via `ove_lvgl_unlock()`).  Use this whenever
 * LVGL API calls are made from a thread other than the dedicated LVGL task.
 *
 * @note Non-copyable and non-movable.
 *
 * Example:
 * @code
 * {
 *     ove::lvgl::LvglGuard guard;
 *     label.text("Hello!");
 * } // lock released here
 * @endcode
 */
class LvglGuard {
public:
	/**
	 * @brief Acquires the LVGL lock, blocking until it is available.
	 */
	LvglGuard() { ove_lvgl_lock(); }

	/**
	 * @brief Releases the LVGL lock.
	 */
	~LvglGuard() { ove_lvgl_unlock(); }

	LvglGuard(const LvglGuard &) = delete;
	LvglGuard &operator=(const LvglGuard &) = delete;
	LvglGuard(LvglGuard &&) = delete;
	LvglGuard &operator=(LvglGuard &&) = delete;
};

/* ================================================================== */
/*  ObjectView — non-owning lv_obj_t* wrapper                         */
/* ================================================================== */

/**
 * @class ObjectView
 * @brief Non-owning, pointer-sized wrapper around an `lv_obj_t*`.
 *
 * `ObjectView` does not manage the lifetime of the underlying LVGL object.
 * It provides safe, ergonomic access to the raw `lv_obj_t*` and exposes a
 * small set of read/query operations.  Widget classes (`Label`, `Bar`,
 * `Box`) inherit from `ObjectView` to gain these capabilities.
 *
 * The class is guaranteed to be pointer-sized (asserted via `static_assert`).
 *
 * @note Destroying an `ObjectView` does NOT delete the underlying LVGL object.
 *       Use `del()` to explicitly delete the LVGL object tree.
 */
class ObjectView {
public:
	/**
	 * @brief Constructs a null `ObjectView` (no associated LVGL object).
	 */
	ObjectView() : obj_(nullptr) {}

	/**
	 * @brief Constructs an `ObjectView` wrapping the given `lv_obj_t*`.
	 * @param[in] obj Pointer to an existing LVGL object; may be null.
	 */
	explicit ObjectView(lv_obj_t *obj) : obj_(obj) {}

	/**
	 * @brief Returns the raw `lv_obj_t*` pointer.
	 * @return Pointer to the wrapped LVGL object, or null.
	 */
	lv_obj_t *get() const { return obj_; }

	/**
	 * @brief Implicit conversion to `lv_obj_t*` for use with LVGL C APIs.
	 * @return Pointer to the wrapped LVGL object.
	 */
	operator lv_obj_t *() const { return obj_; }

	/**
	 * @brief Contextual bool conversion — `true` if the object pointer is non-null.
	 * @return `true` when wrapping a valid LVGL object.
	 */
	explicit operator bool() const { return obj_ != nullptr; }

	/**
	 * @brief Returns an `ObjectView` wrapping the parent of this object.
	 * @return `ObjectView` of the parent, or a null view if there is none.
	 */
	ObjectView parent() const {
		return ObjectView(lv_obj_get_parent(obj_));
	}

	/**
	 * @brief Returns the number of direct children of this object.
	 * @return Child count as a `uint32_t`.
	 */
	uint32_t child_count() const {
		return lv_obj_get_child_count(obj_);
	}

	/**
	 * @brief Returns the current rendered width of this object.
	 * @return Width in pixels.
	 */
	int32_t get_width() const { return lv_obj_get_width(obj_); }

	/**
	 * @brief Returns the current rendered height of this object.
	 * @return Height in pixels.
	 */
	int32_t get_height() const { return lv_obj_get_height(obj_); }

	/**
	 * @brief Deletes the LVGL object and all its children, then nulls the pointer.
	 *
	 * After calling this method the `ObjectView` is in a null/invalid state.
	 */
	void del() { lv_obj_delete(obj_); obj_ = nullptr; }

	/**
	 * @brief Deletes all children of this object without deleting the object itself.
	 */
	void clean() { lv_obj_clean(obj_); }

	/**
	 * @brief Returns an `ObjectView` wrapping the currently active screen.
	 * @return `ObjectView` of the active LVGL screen.
	 */
	static ObjectView screen_active() {
		return ObjectView(lv_screen_active());
	}

protected:
	/** @brief Raw pointer to the wrapped LVGL object. */
	lv_obj_t *obj_;
};

static_assert(sizeof(ObjectView) == sizeof(void *),
	      "ObjectView must be pointer-sized");

/* ================================================================== */
/*  ObjectMixin<Derived> — fluent setters via CRTP                    */
/* ================================================================== */

/**
 * @class ObjectMixin
 * @brief CRTP mixin that adds fluent layout and visibility setters to widget classes.
 *
 * Derive from `ObjectMixin<Derived>` to gain a chainable API for the most
 * common LVGL object properties (size, position, alignment, flags, state,
 * user data, clickability).  All methods return `Derived &` to support
 * method chaining.
 *
 * @tparam Derived The concrete widget class inheriting this mixin.
 *                 Must expose a `get()` method returning `lv_obj_t*`.
 */
template <typename Derived>
class ObjectMixin {
public:
	/**
	 * @brief Sets both width and height of the object.
	 * @param[in] w Width in pixels (or `LV_SIZE_CONTENT` / `LV_PCT(...)`).
	 * @param[in] h Height in pixels.
	 * @return Reference to the derived object for method chaining.
	 */
	Derived &size(int32_t w, int32_t h) {
		lv_obj_set_size(self().get(), w, h);
		return self();
	}

	/**
	 * @brief Sets the width of the object.
	 * @param[in] w Width in pixels.
	 * @return Reference to the derived object for method chaining.
	 */
	Derived &width(int32_t w) {
		lv_obj_set_width(self().get(), w);
		return self();
	}

	/**
	 * @brief Sets the height of the object.
	 * @param[in] h Height in pixels.
	 * @return Reference to the derived object for method chaining.
	 */
	Derived &height(int32_t h) {
		lv_obj_set_height(self().get(), h);
		return self();
	}

	/**
	 * @brief Sets the position of the object relative to its parent.
	 * @param[in] x X coordinate in pixels.
	 * @param[in] y Y coordinate in pixels.
	 * @return Reference to the derived object for method chaining.
	 */
	Derived &pos(int32_t x, int32_t y) {
		lv_obj_set_pos(self().get(), x, y);
		return self();
	}

	/**
	 * @brief Centers the object within its parent.
	 * @return Reference to the derived object for method chaining.
	 */
	Derived &center() {
		lv_obj_center(self().get());
		return self();
	}

	/**
	 * @brief Aligns the object to an anchor with an optional offset.
	 * @param[in] a     Alignment value (e.g., `LV_ALIGN_CENTER`).
	 * @param[in] x_ofs Horizontal offset in pixels (default: 0).
	 * @param[in] y_ofs Vertical offset in pixels (default: 0).
	 * @return Reference to the derived object for method chaining.
	 */
	Derived &align(lv_align_t a, int32_t x_ofs = 0, int32_t y_ofs = 0) {
		lv_obj_align(self().get(), a, x_ofs, y_ofs);
		return self();
	}

	/**
	 * @brief Hides the object by adding `LV_OBJ_FLAG_HIDDEN`.
	 * @return Reference to the derived object for method chaining.
	 */
	Derived &hide() {
		lv_obj_add_flag(self().get(), LV_OBJ_FLAG_HIDDEN);
		return self();
	}

	/**
	 * @brief Shows the object by removing `LV_OBJ_FLAG_HIDDEN`.
	 * @return Reference to the derived object for method chaining.
	 */
	Derived &show() {
		lv_obj_remove_flag(self().get(), LV_OBJ_FLAG_HIDDEN);
		return self();
	}

	/**
	 * @brief Conditionally shows or hides the object.
	 * @param[in] v `true` to show, `false` to hide.
	 * @return Reference to the derived object for method chaining.
	 */
	Derived &visible(bool v) { return v ? show() : hide(); }

	/**
	 * @brief Adds one or more object flags.
	 * @param[in] f Flag bitmask (e.g., `LV_OBJ_FLAG_CLICKABLE`).
	 * @return Reference to the derived object for method chaining.
	 */
	Derived &add_flag(lv_obj_flag_t f) {
		lv_obj_add_flag(self().get(), f);
		return self();
	}

	/**
	 * @brief Removes one or more object flags.
	 * @param[in] f Flag bitmask to clear.
	 * @return Reference to the derived object for method chaining.
	 */
	Derived &remove_flag(lv_obj_flag_t f) {
		lv_obj_remove_flag(self().get(), f);
		return self();
	}

	/**
	 * @brief Adds one or more object states.
	 * @param[in] s State bitmask (e.g., `LV_STATE_CHECKED`).
	 * @return Reference to the derived object for method chaining.
	 */
	Derived &add_state(lv_state_t s) {
		lv_obj_add_state(self().get(), s);
		return self();
	}

	/**
	 * @brief Removes one or more object states.
	 * @param[in] s State bitmask to clear.
	 * @return Reference to the derived object for method chaining.
	 */
	Derived &remove_state(lv_state_t s) {
		lv_obj_remove_state(self().get(), s);
		return self();
	}

	/**
	 * @brief Stores an arbitrary user data pointer on the LVGL object.
	 * @param[in] data Opaque pointer to associate with the object.
	 * @return Reference to the derived object for method chaining.
	 */
	Derived &user_data(void *data) {
		lv_obj_set_user_data(self().get(), data);
		return self();
	}

	/**
	 * @brief Enables or disables click events on the object.
	 * @param[in] on `true` to make the object clickable, `false` to disable.
	 * @return Reference to the derived object for method chaining.
	 */
	Derived &clickable(bool on) {
		if (on)
			lv_obj_add_flag(self().get(), LV_OBJ_FLAG_CLICKABLE);
		else
			lv_obj_remove_flag(self().get(), LV_OBJ_FLAG_CLICKABLE);
		return self();
	}

	/**
	 * @brief Configures this object as a grid container.
	 *
	 * Both arrays must terminate with `LV_GRID_TEMPLATE_LAST`. Track sizes
	 * can be fixed pixel values, `LV_GRID_CONTENT`, or `LV_GRID_FR(n)` for
	 * fractional units. Switches the object's layout to `LV_LAYOUT_GRID`.
	 *
	 * @param[in] cols Column track descriptor array.
	 * @param[in] rows Row track descriptor array.
	 * @return Reference to the derived object for method chaining.
	 */
	Derived &grid_dsc(const int32_t *cols, const int32_t *rows) {
		lv_obj_set_grid_dsc_array(self().get(), cols, rows);
		return self();
	}

	/**
	 * @brief Places this object into a cell of its parent grid.
	 * @param[in] col_align  Horizontal cell alignment (`LV_GRID_ALIGN_*`).
	 * @param[in] col_pos    Zero-based column index.
	 * @param[in] col_span   Number of columns to span (default 1).
	 * @param[in] row_align  Vertical cell alignment (`LV_GRID_ALIGN_*`).
	 * @param[in] row_pos    Zero-based row index.
	 * @param[in] row_span   Number of rows to span (default 1).
	 * @return Reference to the derived object for method chaining.
	 */
	Derived &grid_cell(lv_grid_align_t col_align, int32_t col_pos,
			   int32_t col_span, lv_grid_align_t row_align,
			   int32_t row_pos, int32_t row_span) {
		lv_obj_set_grid_cell(self().get(), col_align, col_pos, col_span,
				     row_align, row_pos, row_span);
		return self();
	}

private:
	Derived &self() { return static_cast<Derived &>(*this); }
};

/* ================================================================== */
/*  EventMixin<Derived> — type-safe event callbacks via CRTP          */
/* ================================================================== */

namespace detail {

/**
 * @brief Concept satisfied by callables that are directly convertible to `void(*)(lv_event_t*)`.
 *
 * Only stateless lambdas and plain function pointers satisfy this concept.
 * Capturing lambdas and functors with state do not, and must use the
 * member-function-pointer overload of `EventMixin::on()` instead.
 *
 * @tparam F The callable type to check.
 */
template <typename F>
concept StatelessCallable = std::is_convertible_v<F, void (*)(lv_event_t *)>;

} /* namespace detail */

/**
 * @class EventMixin
 * @brief CRTP mixin that adds type-safe LVGL event registration to widget classes.
 *
 * Provides overloaded `on()` methods for:
 * - Stateless (non-capturing) lambdas and function pointers, registered
 *   directly as LVGL callbacks without any indirection.
 * - Member function pointers as non-type template parameters, using a
 *   zero-cost trampoline that reads the instance from the LVGL user-data slot.
 *
 * Convenience wrappers `on_click()` and `on_value_changed()` cover the most
 * common event codes.
 *
 * @tparam Derived The concrete widget class inheriting this mixin.
 *                 Must expose a `get()` method returning `lv_obj_t*`.
 */
template <typename Derived>
class EventMixin {
public:
	/**
	 * @brief Registers a stateless callback for the given event code.
	 *
	 * Only participates in overload resolution when `F` satisfies
	 * `detail::StatelessCallable` (i.e., non-capturing lambda or function pointer).
	 *
	 * @tparam F  Callable type satisfying `detail::StatelessCallable`.
	 * @param[in] code Event code to listen for (e.g., `LV_EVENT_CLICKED`).
	 * @param[in] fn   Callback function; stored as a raw function pointer.
	 * @return Reference to the derived object for method chaining.
	 */
	template <detail::StatelessCallable F>
	Derived &on(lv_event_code_t code, F &&fn) {
		lv_obj_add_event_cb(self().get(),
				    static_cast<lv_event_cb_t>(fn),
				    code, nullptr);
		return self();
	}

	/**
	 * @brief Registers a member function pointer as an event callback — zero-cost trampoline.
	 *
	 * The instance pointer is stored in the LVGL user-data slot and recovered
	 * inside the inline trampoline lambda.  No heap allocation is needed.
	 *
	 * @tparam MemFn  Non-type template parameter: pointer to member function with
	 *                signature `void T::fn(lv_event_t*)`.
	 * @tparam T      Class type that owns `MemFn`.
	 * @param[in] code     Event code to listen for.
	 * @param[in] instance Pointer to the object on which `MemFn` will be called.
	 * @return Reference to the derived object for method chaining.
	 */
	template <auto MemFn, typename T>
	Derived &on(lv_event_code_t code, T *instance) {
		lv_obj_add_event_cb(self().get(),
				    [](lv_event_t *e) {
					    auto *self = static_cast<T *>(
						    lv_event_get_user_data(e));
					    (self->*MemFn)(e);
				    },
				    code, instance);
		return self();
	}

	/**
	 * @brief Registers a stateless click callback (shorthand for `on(LV_EVENT_CLICKED, fn)`).
	 * @tparam F Callable type satisfying `detail::StatelessCallable`.
	 * @param[in] fn Callback function invoked on click.
	 * @return Reference to the derived object for method chaining.
	 */
	template <detail::StatelessCallable F>
	Derived &on_click(F &&fn) {
		return on(LV_EVENT_CLICKED, static_cast<F &&>(fn));
	}

	/**
	 * @brief Registers a member function click callback.
	 * @tparam MemFn Non-type member function pointer.
	 * @tparam T     Class type owning `MemFn`.
	 * @param[in] instance Pointer to the instance on which `MemFn` is called.
	 * @return Reference to the derived object for method chaining.
	 */
	template <auto MemFn, typename T>
	Derived &on_click(T *instance) {
		return on<MemFn>(LV_EVENT_CLICKED, instance);
	}

	/**
	 * @brief Registers a stateless value-changed callback (shorthand for `on(LV_EVENT_VALUE_CHANGED, fn)`).
	 * @tparam F Callable type satisfying `detail::StatelessCallable`.
	 * @param[in] fn Callback function invoked when the widget value changes.
	 * @return Reference to the derived object for method chaining.
	 */
	template <detail::StatelessCallable F>
	Derived &on_value_changed(F &&fn) {
		return on(LV_EVENT_VALUE_CHANGED, static_cast<F &&>(fn));
	}

	/**
	 * @brief Registers a member function value-changed callback.
	 * @tparam MemFn Non-type member function pointer.
	 * @tparam T     Class type owning `MemFn`.
	 * @param[in] instance Pointer to the instance on which `MemFn` is called.
	 * @return Reference to the derived object for method chaining.
	 */
	template <auto MemFn, typename T>
	Derived &on_value_changed(T *instance) {
		return on<MemFn>(LV_EVENT_VALUE_CHANGED, instance);
	}

private:
	Derived &self() { return static_cast<Derived &>(*this); }
};

/* ================================================================== */
/*  StyleMixin<Derived> — inline style setters via CRTP               */
/* ================================================================== */

/**
 * @class StyleMixin
 * @brief CRTP mixin that adds inline per-object style setters to widget classes.
 *
 * Each method applies a single style property directly to the `LV_PART_MAIN`
 * part of the underlying LVGL object and returns `Derived &` to support
 * method chaining.  These are convenience wrappers around the LVGL
 * `lv_obj_set_style_*` family of functions.
 *
 * @tparam Derived The concrete widget class inheriting this mixin.
 *                 Must expose a `get()` method returning `lv_obj_t*`.
 */
template <typename Derived>
class StyleMixin {
public:
	/**
	 * @brief Sets the background color of the object.
	 * @param[in] c Background color.
	 * @return Reference to the derived object for method chaining.
	 */
	Derived &bg_color(lv_color_t c) {
		lv_obj_set_style_bg_color(self().get(), c, LV_PART_MAIN);
		return self();
	}

	/**
	 * @brief Sets the background color of a specific part
	 *        (e.g. `LV_PART_INDICATOR`, `LV_PART_KNOB`, `LV_PART_SCROLLBAR`).
	 */
	Derived &bg_color(lv_color_t c, lv_style_selector_t part) {
		lv_obj_set_style_bg_color(self().get(), c, part);
		return self();
	}

	/**
	 * @brief Sets the background opacity.
	 * @param[in] opa Opacity value (0–255, or `LV_OPA_*` constants).
	 * @return Reference to the derived object for method chaining.
	 */
	Derived &bg_opa(lv_opa_t opa) {
		lv_obj_set_style_bg_opa(self().get(), opa, LV_PART_MAIN);
		return self();
	}

	/** @brief Sets the background opacity on a specific part. */
	Derived &bg_opa(lv_opa_t opa, lv_style_selector_t part) {
		lv_obj_set_style_bg_opa(self().get(), opa, part);
		return self();
	}

	/**
	 * @brief Sets the border color.
	 * @param[in] c Border color.
	 * @return Reference to the derived object for method chaining.
	 */
	Derived &border_color(lv_color_t c) {
		lv_obj_set_style_border_color(self().get(), c, LV_PART_MAIN);
		return self();
	}

	/**
	 * @brief Sets the border width in pixels.
	 * @param[in] w Border width.
	 * @return Reference to the derived object for method chaining.
	 */
	Derived &border_width(int32_t w) {
		lv_obj_set_style_border_width(self().get(), w, LV_PART_MAIN);
		return self();
	}

	/** @brief Sets the border width on a specific part. */
	Derived &border_width(int32_t w, lv_style_selector_t part) {
		lv_obj_set_style_border_width(self().get(), w, part);
		return self();
	}

	/**
	 * @brief Sets the corner radius.
	 * @param[in] r Radius in pixels; use `LV_RADIUS_CIRCLE` for a full circle.
	 * @return Reference to the derived object for method chaining.
	 */
	Derived &radius(int32_t r) {
		lv_obj_set_style_radius(self().get(), r, LV_PART_MAIN);
		return self();
	}

	/** @brief Sets the corner radius on a specific part. */
	Derived &radius(int32_t r, lv_style_selector_t part) {
		lv_obj_set_style_radius(self().get(), r, part);
		return self();
	}

	/**
	 * @brief Sets uniform padding on all four sides.
	 * @param[in] p Padding in pixels.
	 * @return Reference to the derived object for method chaining.
	 */
	Derived &pad_all(int32_t p) {
		lv_obj_set_style_pad_all(self().get(), p, LV_PART_MAIN);
		return self();
	}

	/**
	 * @brief Sets horizontal (left + right) padding.
	 * @param[in] p Padding in pixels.
	 * @return Reference to the derived object for method chaining.
	 */
	Derived &pad_hor(int32_t p) {
		lv_obj_set_style_pad_hor(self().get(), p, LV_PART_MAIN);
		return self();
	}

	/**
	 * @brief Sets vertical (top + bottom) padding.
	 * @param[in] p Padding in pixels.
	 * @return Reference to the derived object for method chaining.
	 */
	Derived &pad_ver(int32_t p) {
		lv_obj_set_style_pad_ver(self().get(), p, LV_PART_MAIN);
		return self();
	}

	/**
	 * @brief Sets the gap between children in flex/grid layouts.
	 * @param[in] g Gap in pixels.
	 * @return Reference to the derived object for method chaining.
	 */
	Derived &pad_gap(int32_t g) {
		lv_obj_set_style_pad_gap(self().get(), g, LV_PART_MAIN);
		return self();
	}

	/**
	 * @brief Sets the text color.
	 * @param[in] c Text color.
	 * @return Reference to the derived object for method chaining.
	 */
	Derived &text_color(lv_color_t c) {
		lv_obj_set_style_text_color(self().get(), c, LV_PART_MAIN);
		return self();
	}

	/**
	 * @brief Sets the font used to render text.
	 * @param[in] f Pointer to an LVGL font descriptor.
	 * @return Reference to the derived object for method chaining.
	 */
	Derived &text_font(const lv_font_t *f) {
		lv_obj_set_style_text_font(self().get(), f, LV_PART_MAIN);
		return self();
	}

private:
	Derived &self() { return static_cast<Derived &>(*this); }
};

/* ================================================================== */
/*  Style — RAII style object                                         */
/* ================================================================== */

/**
 * @class Style
 * @brief RAII wrapper around an `lv_style_t` object.
 *
 * Initialises the LVGL style on construction and resets (frees) it on
 * destruction.  Provides a fluent, chainable API for the most common
 * style properties.  Once populated, a `Style` can be applied to one or
 * more LVGL objects via `lv_obj_add_style()`.
 *
 * @note Non-copyable; movable.  On move, the source style is left in a
 *       freshly initialised (empty) state rather than null so that LVGL
 *       does not hold a dangling pointer.
 */
class Style {
public:
	/**
	 * @brief Constructs and initialises an empty LVGL style.
	 */
	Style() { lv_style_init(&style_); }

	/**
	 * @brief Destroys the style, releasing any resources held by LVGL.
	 */
	~Style() { lv_style_reset(&style_); }

	Style(const Style &) = delete;
	Style &operator=(const Style &) = delete;

	/**
	 * @brief Move constructor — transfers the style data and reinitialises the source.
	 * @param other The source style; left in a valid, empty state after the move.
	 */
	Style(Style &&other) noexcept : style_(other.style_) {
		lv_style_init(&other.style_);
	}

	/**
	 * @brief Move-assignment operator — transfers the style data and reinitialises the source.
	 * @param other The source style; left in a valid, empty state after the move.
	 * @return Reference to this object.
	 */
	Style &operator=(Style &&other) noexcept {
		if (this != &other) {
			lv_style_reset(&style_);
			style_ = other.style_;
			lv_style_init(&other.style_);
		}
		return *this;
	}

	/**
	 * @brief Returns a mutable pointer to the underlying `lv_style_t`.
	 * @return Pointer to the internal LVGL style struct.
	 */
	lv_style_t *get() { return &style_; }

	/**
	 * @brief Returns a const pointer to the underlying `lv_style_t`.
	 * @return Const pointer to the internal LVGL style struct.
	 */
	const lv_style_t *get() const { return &style_; }

	/**
	 * @brief Sets the background color.
	 * @param[in] c Background color.
	 * @return Reference to this style for method chaining.
	 */
	Style &bg_color(lv_color_t c) {
		lv_style_set_bg_color(&style_, c);
		return *this;
	}

	/**
	 * @brief Sets the background opacity.
	 * @param[in] opa Opacity value (0–255).
	 * @return Reference to this style for method chaining.
	 */
	Style &bg_opa(lv_opa_t opa) {
		lv_style_set_bg_opa(&style_, opa);
		return *this;
	}

	/**
	 * @brief Sets the corner radius.
	 * @param[in] r Radius in pixels.
	 * @return Reference to this style for method chaining.
	 */
	Style &radius(int32_t r) {
		lv_style_set_radius(&style_, r);
		return *this;
	}

	/**
	 * @brief Sets the border color.
	 * @param[in] c Border color.
	 * @return Reference to this style for method chaining.
	 */
	Style &border_color(lv_color_t c) {
		lv_style_set_border_color(&style_, c);
		return *this;
	}

	/**
	 * @brief Sets the border width in pixels.
	 * @param[in] w Border width.
	 * @return Reference to this style for method chaining.
	 */
	Style &border_width(int32_t w) {
		lv_style_set_border_width(&style_, w);
		return *this;
	}

	/**
	 * @brief Sets uniform padding on all four sides.
	 * @param[in] p Padding in pixels.
	 * @return Reference to this style for method chaining.
	 */
	Style &pad_all(int32_t p) {
		lv_style_set_pad_all(&style_, p);
		return *this;
	}

	/**
	 * @brief Sets the text color.
	 * @param[in] c Text color.
	 * @return Reference to this style for method chaining.
	 */
	Style &text_color(lv_color_t c) {
		lv_style_set_text_color(&style_, c);
		return *this;
	}

	/**
	 * @brief Sets the font used to render text.
	 * @param[in] f Pointer to an LVGL font descriptor.
	 * @return Reference to this style for method chaining.
	 */
	Style &text_font(const lv_font_t *f) {
		lv_style_set_text_font(&style_, f);
		return *this;
	}

private:
	lv_style_t style_;
};

/* ================================================================== */
/*  State<T> — reactive state (requires LV_USE_OBSERVER)              */
/* ================================================================== */

#if LV_USE_OBSERVER

/**
 * @class State
 * @brief Reactive integer state backed by the LVGL observer subsystem.
 *
 * `State<T>` wraps an `lv_subject_t` (an observable integer slot) and
 * exposes a value-like interface.  Widget properties (e.g., label text) can
 * be bound to a `State` so that they update automatically whenever `set()`
 * is called.
 *
 * Requires `LV_USE_OBSERVER` to be enabled in `lv_conf.h`.
 *
 * @tparam T An integral type (constrained by `std::integral`).
 *           Values are cast to/from `int32_t` internally.
 *
 * @note Non-copyable.
 */
template <std::integral T>
class State {
public:
	/**
	 * @brief Constructs the reactive state with an initial integer value.
	 * @param[in] initial Starting value (default: 0).
	 */
	explicit State(T initial = 0) {
		lv_subject_init_int(&subject_, static_cast<int32_t>(initial));
	}

	/**
	 * @brief Destroys the state and deinitialises the underlying LVGL subject.
	 */
	~State() { lv_subject_deinit(&subject_); }

	State(const State &) = delete;
	State &operator=(const State &) = delete;

	/**
	 * @brief Sets a new value and notifies all bound observers.
	 * @param[in] val New value to store.
	 */
	void set(T val) { lv_subject_set_int(&subject_, static_cast<int32_t>(val)); }

	/**
	 * @brief Reads the current value.
	 * @return The current value cast to `T`.
	 */
	T get() const {
		return static_cast<T>(lv_subject_get_int(
			const_cast<lv_subject_t *>(&subject_)));
	}

	/**
	 * @brief Implicit conversion to `T` — equivalent to calling `get()`.
	 * @return The current value.
	 */
	operator T() const { return get(); }

	/**
	 * @brief Pre-increment operator — increments the value by one and notifies observers.
	 * @return Reference to this state.
	 */
	State &operator++() { set(get() + 1); return *this; }

	/**
	 * @brief Pre-decrement operator — decrements the value by one and notifies observers.
	 * @return Reference to this state.
	 */
	State &operator--() { set(get() - 1); return *this; }

	/**
	 * @brief Returns a pointer to the underlying `lv_subject_t` for use with LVGL bind APIs.
	 * @return Pointer to the internal LVGL subject.
	 */
	lv_subject_t *subject() { return &subject_; }

private:
	lv_subject_t subject_;
};


#endif /* LV_USE_OBSERVER */

/* ================================================================== */
/*  Widget wrappers — Label, Bar, Box                                 */
/* ================================================================== */

/**
 * @class Label
 * @brief C++ wrapper for an LVGL label widget.
 *
 * `Label` combines `ObjectView`, `ObjectMixin`, `EventMixin`, and
 * `StyleMixin` to provide a fully fluent, type-safe API for LVGL label
 * objects.  All instances are pointer-sized (asserted via `static_assert`).
 *
 * Create a label using the static factory method `Label::create(parent)`.
 *
 * When `LV_USE_OBSERVER` is enabled, labels can be bound to a `State<T>`
 * object so that the displayed text updates automatically.
 *
 * @note Does not own the underlying LVGL object.  Use `del()` to destroy it.
 */
class Label : public ObjectView,
	      public ObjectMixin<Label>,
	      public EventMixin<Label>,
	      public StyleMixin<Label> {
public:
	/**
	 * @brief Constructs a `Label` wrapping an existing LVGL label object.
	 * @param[in] obj Pointer to an LVGL label created via `lv_label_create()`.
	 */
	explicit Label(lv_obj_t *obj) : ObjectView(obj) {}

	/**
	 * @brief Factory method — creates a new LVGL label as a child of `parent`.
	 * @param[in] parent The parent `ObjectView`.
	 * @return A `Label` wrapping the newly created LVGL label.
	 */
	static Label create(ObjectView parent) {
		return Label(lv_label_create(parent));
	}

	/**
	 * @brief Sets the label text by copying the string (LVGL allocates internally).
	 * @param[in] txt Null-terminated string to display.
	 * @return Reference to this label for method chaining.
	 */
	Label &text(const char *txt) {
		lv_label_set_text(obj_, txt);
		return *this;
	}

	/**
	 * @brief Sets the label text to a static string (no copy — pointer must remain valid).
	 *
	 * The string pointed to by `txt` must remain valid for the lifetime of
	 * the label.  Avoids a heap allocation in LVGL.
	 *
	 * @param[in] txt Pointer to a static or otherwise persistent string.
	 * @return Reference to this label for method chaining.
	 */
	Label &text_static(const char *txt) {
		lv_label_set_text_static(obj_, txt);
		return *this;
	}

	/**
	 * @brief Sets the label text using a printf-style format string.
	 *
	 * The formatted result is written to a 128-byte stack buffer before being
	 * passed to LVGL.  Strings longer than 127 characters will be truncated.
	 *
	 * @param[in] fmt printf-style format string.
	 * @param[in] ... Additional arguments matching the format string.
	 * @return Reference to this label for method chaining.
	 */
	Label &text_fmt(const char *fmt, ...) {
		va_list args;
		va_start(args, fmt);
		/* LVGL doesn't have a va_list variant, use snprintf + set */
		char buf[128];
		vsnprintf(buf, sizeof(buf), fmt, args);
		va_end(args);
		lv_label_set_text(obj_, buf);
		return *this;
	}

	/**
	 * @brief Sets the font used to render the label text.
	 * @param[in] f Pointer to an LVGL font descriptor.
	 * @return Reference to this label for method chaining.
	 */
	Label &font(const lv_font_t *f) {
		lv_obj_set_style_text_font(obj_, f, LV_PART_MAIN);
		return *this;
	}

	/**
	 * @brief Sets the text color.
	 * @param[in] c Text color.
	 * @return Reference to this label for method chaining.
	 */
	Label &color(lv_color_t c) {
		lv_obj_set_style_text_color(obj_, c, LV_PART_MAIN);
		return *this;
	}

	/**
	 * @brief Sets the long-text mode (wrap, scroll, clip, etc.).
	 * @param[in] mode One of the `lv_label_long_mode_t` constants.
	 * @return Reference to this label for method chaining.
	 */
	Label &long_mode(lv_label_long_mode_t mode) {
		lv_label_set_long_mode(obj_, mode);
		return *this;
	}

#if LV_USE_OBSERVER
	/**
	 * @brief Binds the label text to a reactive `State<T>` value.
	 *
	 * The label text is updated automatically whenever the state value changes,
	 * using `fmt` as the printf format string (e.g., `"%d"`).
	 *
	 * Requires `LV_USE_OBSERVER`.
	 *
	 * @tparam T Integral type of the state value.
	 * @param[in] state Reference to the `State<T>` to observe.
	 * @param[in] fmt   printf format string used to render the integer value.
	 * @return Reference to this label for method chaining.
	 */
	template <std::integral T>
	Label &bind_text(State<T> &state, const char *fmt) {
		lv_label_bind_text(obj_, state.subject(), fmt);
		return *this;
	}
#endif
};

static_assert(sizeof(Label) == sizeof(void *),
	      "Label must be pointer-sized");

/**
 * @class Bar
 * @brief C++ wrapper for an LVGL progress-bar widget.
 *
 * `Bar` combines `ObjectView`, `ObjectMixin`, `EventMixin`, and
 * `StyleMixin` to provide a fully fluent API for LVGL bar objects.  All
 * instances are pointer-sized (asserted via `static_assert`).
 *
 * Create a bar using the static factory method `Bar::create(parent)`.
 *
 * @note Does not own the underlying LVGL object.  Use `del()` to destroy it.
 * @note `lv_bar_bind_value()` is not available in LVGL 9.2–9.3 and is
 *       therefore absent from this wrapper.
 */
class Bar : public ObjectView,
	    public ObjectMixin<Bar>,
	    public EventMixin<Bar>,
	    public StyleMixin<Bar> {
public:
	/**
	 * @brief Constructs a `Bar` wrapping an existing LVGL bar object.
	 * @param[in] obj Pointer to an LVGL bar created via `lv_bar_create()`.
	 */
	explicit Bar(lv_obj_t *obj) : ObjectView(obj) {}

	/**
	 * @brief Factory method — creates a new LVGL bar as a child of `parent`.
	 * @param[in] parent The parent `ObjectView`.
	 * @return A `Bar` wrapping the newly created LVGL bar.
	 */
	static Bar create(ObjectView parent) {
		return Bar(lv_bar_create(parent));
	}

	/**
	 * @brief Sets the current value of the bar with animation enabled.
	 * @param[in] val New value (should be within the range set by `range()`).
	 * @return Reference to this bar for method chaining.
	 */
	Bar &value(int32_t val) {
		lv_bar_set_value(obj_, val, LV_ANIM_ON);
		return *this;
	}

	/**
	 * @brief Sets the current value of the bar with explicit animation control.
	 * @param[in] val  New value.
	 * @param[in] anim `LV_ANIM_ON` to animate the transition, `LV_ANIM_OFF` to jump immediately.
	 * @return Reference to this bar for method chaining.
	 */
	Bar &value(int32_t val, lv_anim_enable_t anim) {
		lv_bar_set_value(obj_, val, anim);
		return *this;
	}

	/**
	 * @brief Sets the minimum and maximum values for the bar.
	 * @param[in] min Minimum value (maps to 0% fill).
	 * @param[in] max Maximum value (maps to 100% fill).
	 * @return Reference to this bar for method chaining.
	 */
	Bar &range(int32_t min, int32_t max) {
		lv_bar_set_range(obj_, min, max);
		return *this;
	}

	/**
	 * @brief Sets the background color of the indicator (filled) part.
	 * @param[in] c Indicator color.
	 * @return Reference to this bar for method chaining.
	 */
	Bar &indicator_color(lv_color_t c) {
		lv_obj_set_style_bg_color(obj_, c, LV_PART_INDICATOR);
		return *this;
	}

	/**
	 * @brief Sets the background color of the bar track (unfilled) part.
	 * @param[in] c Track background color.
	 * @return Reference to this bar for method chaining.
	 */
	Bar &bar_color(lv_color_t c) {
		lv_obj_set_style_bg_color(obj_, c, LV_PART_MAIN);
		return *this;
	}

	/* Note: lv_bar_bind_value() does NOT exist in LVGL 9.2-9.3 */
};

static_assert(sizeof(Bar) == sizeof(void *),
	      "Bar must be pointer-sized");

/**
 * @class Box
 * @brief C++ wrapper for a generic LVGL container object with scrolling disabled.
 *
 * `Box` is a plain `lv_obj_t` container created with the scrollable flag
 * cleared, making it behave as a simple layout box.  It supports the full
 * fluent API from `ObjectMixin`, `EventMixin`, and `StyleMixin`.
 *
 * Use the `vbox()` and `hbox()` free functions for pre-configured vertical
 * and horizontal flex containers.
 *
 * @note Does not own the underlying LVGL object.  Use `del()` to destroy it.
 */
class Box : public ObjectView,
	    public ObjectMixin<Box>,
	    public EventMixin<Box>,
	    public StyleMixin<Box> {
public:
	/**
	 * @brief Constructs a `Box` wrapping an existing LVGL object.
	 * @param[in] obj Pointer to an LVGL object.
	 */
	explicit Box(lv_obj_t *obj) : ObjectView(obj) {}

	/**
	 * @brief Factory method — creates a new LVGL container with scrolling disabled.
	 * @param[in] parent The parent `ObjectView`.
	 * @return A `Box` wrapping the newly created LVGL object.
	 */
	static Box create(ObjectView parent) {
		lv_obj_t *obj = lv_obj_create(parent);
		lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
		return Box(obj);
	}
};

static_assert(sizeof(Box) == sizeof(void *),
	      "Box must be pointer-sized");

/**
 * @class Button
 * @brief C++ wrapper for an LVGL button widget.
 *
 * A button is a clickable container. Use `toggle_mode(true)` to turn it
 * into a checkable toggle whose state is tracked via `LV_STATE_CHECKED`.
 * Put a `Label` child inside to give it text.
 */
class Button : public ObjectView,
	       public ObjectMixin<Button>,
	       public EventMixin<Button>,
	       public StyleMixin<Button> {
public:
	explicit Button(lv_obj_t *obj) : ObjectView(obj) {}

	/** @brief Creates a new button as a child of `parent`. */
	static Button create(ObjectView parent) {
		return Button(lv_button_create(parent));
	}

	/**
	 * @brief Enables or disables toggle (checkable) behaviour.
	 * @param[in] on `true` to make the button maintain a checked state.
	 */
	Button &toggle_mode(bool on) {
		if (on)
			lv_obj_add_flag(obj_, LV_OBJ_FLAG_CHECKABLE);
		else
			lv_obj_remove_flag(obj_, LV_OBJ_FLAG_CHECKABLE);
		return *this;
	}

	/** @brief Sets the checked state explicitly. */
	Button &checked(bool v) {
		if (v) lv_obj_add_state(obj_, LV_STATE_CHECKED);
		else   lv_obj_remove_state(obj_, LV_STATE_CHECKED);
		return *this;
	}

	/** @brief Returns `true` if the button is currently in the checked state. */
	bool is_checked() const {
		return lv_obj_has_state(obj_, LV_STATE_CHECKED);
	}
};

static_assert(sizeof(Button) == sizeof(void *),
	      "Button must be pointer-sized");

/**
 * @class Slider
 * @brief C++ wrapper for an LVGL slider widget (interactive bar).
 *
 * Inherits the bar value/range shape and adds knob styling. Listen for
 * changes with `on_value_changed(...)`.
 */
class Slider : public ObjectView,
	       public ObjectMixin<Slider>,
	       public EventMixin<Slider>,
	       public StyleMixin<Slider> {
public:
	explicit Slider(lv_obj_t *obj) : ObjectView(obj) {}

	static Slider create(ObjectView parent) {
		return Slider(lv_slider_create(parent));
	}

	/** @brief Sets the current value (animated). */
	Slider &value(int32_t val) {
		lv_slider_set_value(obj_, val, LV_ANIM_ON);
		return *this;
	}

	/** @brief Sets the current value with explicit animation control. */
	Slider &value(int32_t val, lv_anim_enable_t anim) {
		lv_slider_set_value(obj_, val, anim);
		return *this;
	}

	/** @brief Sets the min/max range. */
	Slider &range(int32_t min, int32_t max) {
		lv_slider_set_range(obj_, min, max);
		return *this;
	}

	/** @brief Returns the current value. */
	int32_t get_value() const { return lv_slider_get_value(obj_); }

	/** @brief Sets the filled indicator colour (`LV_PART_INDICATOR`). */
	Slider &indicator_color(lv_color_t c) {
		lv_obj_set_style_bg_color(obj_, c, LV_PART_INDICATOR);
		return *this;
	}

	/** @brief Sets the knob colour (`LV_PART_KNOB`). */
	Slider &knob_color(lv_color_t c) {
		lv_obj_set_style_bg_color(obj_, c, LV_PART_KNOB);
		return *this;
	}
};

static_assert(sizeof(Slider) == sizeof(void *),
	      "Slider must be pointer-sized");

/**
 * @class Switch
 * @brief C++ wrapper for an LVGL switch widget (binary toggle).
 *
 * State is tracked via `LV_STATE_CHECKED`. Listen for changes with
 * `on_value_changed(...)`.
 */
class Switch : public ObjectView,
	       public ObjectMixin<Switch>,
	       public EventMixin<Switch>,
	       public StyleMixin<Switch> {
public:
	explicit Switch(lv_obj_t *obj) : ObjectView(obj) {}

	static Switch create(ObjectView parent) {
		return Switch(lv_switch_create(parent));
	}

	/** @brief Sets the checked (on) state. */
	Switch &checked(bool v) {
		if (v) lv_obj_add_state(obj_, LV_STATE_CHECKED);
		else   lv_obj_remove_state(obj_, LV_STATE_CHECKED);
		return *this;
	}

	/** @brief Returns `true` if the switch is on. */
	bool is_checked() const {
		return lv_obj_has_state(obj_, LV_STATE_CHECKED);
	}
};

static_assert(sizeof(Switch) == sizeof(void *),
	      "Switch must be pointer-sized");

/**
 * @class Checkbox
 * @brief C++ wrapper for an LVGL checkbox (labelled binary toggle).
 *
 * State is tracked via `LV_STATE_CHECKED` exactly like `Switch`; `text()`
 * sets the label shown alongside the tick box.
 */
class Checkbox : public ObjectView,
		 public ObjectMixin<Checkbox>,
		 public EventMixin<Checkbox>,
		 public StyleMixin<Checkbox> {
public:
	explicit Checkbox(lv_obj_t *obj) : ObjectView(obj) {}

	static Checkbox create(ObjectView parent) {
		return Checkbox(lv_checkbox_create(parent));
	}

	/** @brief Sets the label text (copies into LVGL-owned memory). */
	Checkbox &text(const char *txt) {
		lv_checkbox_set_text(obj_, txt);
		return *this;
	}

	/** @brief Sets the label text from a persistent static string (no copy). */
	Checkbox &text_static(const char *txt) {
		lv_checkbox_set_text_static(obj_, txt);
		return *this;
	}

	/** @brief Sets the checked state. */
	Checkbox &checked(bool v) {
		if (v) lv_obj_add_state(obj_, LV_STATE_CHECKED);
		else   lv_obj_remove_state(obj_, LV_STATE_CHECKED);
		return *this;
	}

	/** @brief Returns `true` if the checkbox is ticked. */
	bool is_checked() const {
		return lv_obj_has_state(obj_, LV_STATE_CHECKED);
	}
};

static_assert(sizeof(Checkbox) == sizeof(void *),
	      "Checkbox must be pointer-sized");

/**
 * @class Arc
 * @brief C++ wrapper for an LVGL arc widget (circular slider / gauge).
 *
 * Arcs expose a value/range much like `Bar` and `Slider`, plus rotation
 * and angle controls. The track uses `arc_color` on `LV_PART_MAIN`, the
 * filled indicator uses `arc_color` on `LV_PART_INDICATOR`, and the
 * draggable knob uses `bg_color` on `LV_PART_KNOB`.
 */
class Arc : public ObjectView,
	    public ObjectMixin<Arc>,
	    public EventMixin<Arc>,
	    public StyleMixin<Arc> {
public:
	explicit Arc(lv_obj_t *obj) : ObjectView(obj) {}

	static Arc create(ObjectView parent) {
		return Arc(lv_arc_create(parent));
	}

	/** @brief Sets the current value. */
	Arc &value(int32_t val) {
		lv_arc_set_value(obj_, val);
		return *this;
	}

	/** @brief Sets the min/max range. */
	Arc &range(int32_t min, int32_t max) {
		lv_arc_set_range(obj_, min, max);
		return *this;
	}

	/** @brief Sets the background arc start/end angles in degrees. */
	Arc &bg_angles(uint32_t start, uint32_t end) {
		lv_arc_set_bg_angles(obj_, start, end);
		return *this;
	}

	/** @brief Sets the foreground indicator start/end angles in degrees. */
	Arc &angles(uint32_t start, uint32_t end) {
		lv_arc_set_angles(obj_, start, end);
		return *this;
	}

	/** @brief Sets the rotation offset of the arc in degrees. */
	Arc &rotation(uint32_t rot) {
		lv_arc_set_rotation(obj_, rot);
		return *this;
	}

	/** @brief Returns the current value. */
	int32_t get_value() const { return lv_arc_get_value(obj_); }

	/** @brief Sets the track arc colour (`LV_PART_MAIN`). */
	Arc &track_color(lv_color_t c) {
		lv_obj_set_style_arc_color(obj_, c, LV_PART_MAIN);
		return *this;
	}

	/** @brief Sets the indicator arc colour (`LV_PART_INDICATOR`). */
	Arc &indicator_color(lv_color_t c) {
		lv_obj_set_style_arc_color(obj_, c, LV_PART_INDICATOR);
		return *this;
	}

	/** @brief Sets the indicator arc width (`LV_PART_INDICATOR`). */
	Arc &indicator_width(int32_t w) {
		lv_obj_set_style_arc_width(obj_, w, LV_PART_INDICATOR);
		return *this;
	}

	/** @brief Sets the knob colour (`LV_PART_KNOB`). */
	Arc &knob_color(lv_color_t c) {
		lv_obj_set_style_bg_color(obj_, c, LV_PART_KNOB);
		return *this;
	}
};

static_assert(sizeof(Arc) == sizeof(void *),
	      "Arc must be pointer-sized");

/**
 * @class Image
 * @brief C++ wrapper for an LVGL image widget.
 *
 * Sources can be built-in descriptors (`const lv_image_dsc_t *`), symbol
 * strings (`LV_SYMBOL_OK` and friends), or file-system paths if a file
 * system driver is registered. Transformation is pivot-aware.
 */
class Image : public ObjectView,
	      public ObjectMixin<Image>,
	      public EventMixin<Image>,
	      public StyleMixin<Image> {
public:
	explicit Image(lv_obj_t *obj) : ObjectView(obj) {}

	static Image create(ObjectView parent) {
		return Image(lv_image_create(parent));
	}

	/** @brief Sets the image source (descriptor, symbol, or path). */
	Image &src(const void *src) {
		lv_image_set_src(obj_, src);
		return *this;
	}

	/** @brief Sets the rotation angle in 0.1 degree units (0–3600). */
	Image &rotation(int32_t angle) {
		lv_image_set_rotation(obj_, angle);
		return *this;
	}

	/** @brief Sets the zoom factor (256 = 1.0x, 512 = 2.0x). */
	Image &scale(uint32_t zoom) {
		lv_image_set_scale(obj_, zoom);
		return *this;
	}

	/** @brief Sets the rotation/scale pivot point. */
	Image &pivot(int32_t x, int32_t y) {
		lv_image_set_pivot(obj_, x, y);
		return *this;
	}
};

static_assert(sizeof(Image) == sizeof(void *),
	      "Image must be pointer-sized");

/**
 * @class Msgbox
 * @brief C++ wrapper for an LVGL message-box (modal dialog).
 *
 * Build up the content with `add_title`, `add_text`, `add_close_button`,
 * and `add_footer_button`. Pass `nullptr` as the parent to center the
 * msgbox on the active screen. Close via `close()` or let the close
 * button handle it.
 */
class Msgbox : public ObjectView,
	       public ObjectMixin<Msgbox>,
	       public EventMixin<Msgbox>,
	       public StyleMixin<Msgbox> {
public:
	explicit Msgbox(lv_obj_t *obj) : ObjectView(obj) {}

	/** @brief Creates a new modal msgbox. Passing a null parent centers
	 *         it on the active screen. */
	static Msgbox create(ObjectView parent) {
		return Msgbox(lv_msgbox_create(parent));
	}

	/** @brief Adds a title in the header, returning the title label. */
	Msgbox &add_title(const char *txt) {
		lv_msgbox_add_title(obj_, txt);
		return *this;
	}

	/** @brief Adds body text, returning the content label. */
	Msgbox &add_text(const char *txt) {
		lv_msgbox_add_text(obj_, txt);
		return *this;
	}

	/** @brief Adds a close (X) button in the header. */
	Msgbox &add_close_button() {
		lv_msgbox_add_close_button(obj_);
		return *this;
	}

	/** @brief Adds a footer button with the given text. Returns an
	 *         ObjectView wrapping the created button so callers can
	 *         attach click handlers. */
	ObjectView add_footer_button(const char *txt) {
		return ObjectView(lv_msgbox_add_footer_button(obj_, txt));
	}

	/** @brief Returns the content container (where body widgets live). */
	ObjectView get_content() const {
		return ObjectView(lv_msgbox_get_content(obj_));
	}

	/** @brief Returns the header container. */
	ObjectView get_header() const {
		return ObjectView(lv_msgbox_get_header(obj_));
	}

	/** @brief Returns the footer container. */
	ObjectView get_footer() const {
		return ObjectView(lv_msgbox_get_footer(obj_));
	}

	/** @brief Closes and deletes the msgbox. */
	void close() { lv_msgbox_close(obj_); }
};

static_assert(sizeof(Msgbox) == sizeof(void *),
	      "Msgbox must be pointer-sized");

/**
 * @class Spinner
 * @brief C++ wrapper for LVGL's circular loading indicator.
 *
 * A Spinner is an Arc with a continuously rotating indicator. Configure
 * via `anim_params(time_ms, arc_length_deg)`.
 */
class Spinner : public ObjectView,
		public ObjectMixin<Spinner>,
		public EventMixin<Spinner>,
		public StyleMixin<Spinner> {
public:
	explicit Spinner(lv_obj_t *obj) : ObjectView(obj) {}

	static Spinner create(ObjectView parent) {
		return Spinner(lv_spinner_create(parent));
	}

	/**
	 * @brief Sets the animation parameters.
	 * @param[in] time_ms   Full rotation period in milliseconds.
	 * @param[in] angle_deg Arc length of the indicator in degrees.
	 */
	Spinner &anim_params(uint32_t time_ms, uint32_t angle_deg) {
		lv_spinner_set_anim_params(obj_, time_ms, angle_deg);
		return *this;
	}
};

static_assert(sizeof(Spinner) == sizeof(void *),
	      "Spinner must be pointer-sized");

/**
 * @class Led
 * @brief C++ wrapper for LVGL's LED widget — a colored indicator circle.
 */
class Led : public ObjectView,
	    public ObjectMixin<Led>,
	    public EventMixin<Led>,
	    public StyleMixin<Led> {
public:
	explicit Led(lv_obj_t *obj) : ObjectView(obj) {}

	static Led create(ObjectView parent) {
		return Led(lv_led_create(parent));
	}

	/** @brief Sets the LED base colour. */
	Led &color(lv_color_t c) {
		lv_led_set_color(obj_, c);
		return *this;
	}

	/** @brief Sets the LED brightness (0–255). */
	Led &brightness(uint8_t bright) {
		lv_led_set_brightness(obj_, bright);
		return *this;
	}

	/** @brief Turns the LED on (full brightness). */
	Led &on() {
		lv_led_on(obj_);
		return *this;
	}

	/** @brief Turns the LED off. */
	Led &off() {
		lv_led_off(obj_);
		return *this;
	}

	/** @brief Toggles between on/off. */
	Led &toggle() {
		lv_led_toggle(obj_);
		return *this;
	}

	/** @brief Returns the current brightness. */
	uint8_t get_brightness() const {
		return lv_led_get_brightness(obj_);
	}
};

static_assert(sizeof(Led) == sizeof(void *),
	      "Led must be pointer-sized");

/**
 * @class Textarea
 * @brief C++ wrapper for an LVGL text-entry widget.
 *
 * Supports single- or multi-line input, placeholder text, password mode,
 * max length, and an accepted-character filter. Pair with `Keyboard` on
 * touch targets; attach to a `Group` on non-touch targets.
 */
class Textarea : public ObjectView,
		 public ObjectMixin<Textarea>,
		 public EventMixin<Textarea>,
		 public StyleMixin<Textarea> {
public:
	explicit Textarea(lv_obj_t *obj) : ObjectView(obj) {}

	static Textarea create(ObjectView parent) {
		return Textarea(lv_textarea_create(parent));
	}

	/** @brief Replaces the current text (LVGL copies the string). */
	Textarea &text(const char *txt) {
		lv_textarea_set_text(obj_, txt);
		return *this;
	}

	/** @brief Appends text to the end. */
	Textarea &add_text(const char *txt) {
		lv_textarea_add_text(obj_, txt);
		return *this;
	}

	/** @brief Sets the placeholder shown while the textarea is empty. */
	Textarea &placeholder(const char *txt) {
		lv_textarea_set_placeholder_text(obj_, txt);
		return *this;
	}

	/** @brief Toggles single-line mode (enter fires `LV_EVENT_READY`). */
	Textarea &one_line(bool en) {
		lv_textarea_set_one_line(obj_, en);
		return *this;
	}

	/** @brief Enables password masking. */
	Textarea &password_mode(bool en) {
		lv_textarea_set_password_mode(obj_, en);
		return *this;
	}

	/** @brief Sets the maximum character count (0 = unlimited). */
	Textarea &max_length(uint32_t n) {
		lv_textarea_set_max_length(obj_, n);
		return *this;
	}

	/** @brief Restricts input to characters in `list`; `nullptr` = any. */
	Textarea &accepted_chars(const char *list) {
		lv_textarea_set_accepted_chars(obj_, list);
		return *this;
	}

	/** @brief Moves the cursor to the given character position. */
	Textarea &cursor_pos(int32_t pos) {
		lv_textarea_set_cursor_pos(obj_, pos);
		return *this;
	}

	/** @brief Enables moving the cursor by clicking. */
	Textarea &cursor_click_pos(bool en) {
		lv_textarea_set_cursor_click_pos(obj_, en);
		return *this;
	}

	/** @brief Returns the current text (pointer owned by LVGL). */
	const char *get_text() const { return lv_textarea_get_text(obj_); }

	/** @brief Returns the current cursor position. */
	uint32_t get_cursor_pos() const {
		return lv_textarea_get_cursor_pos(obj_);
	}

	/** @brief Returns `true` if password mode is enabled. */
	bool is_password_mode() const {
		return lv_textarea_get_password_mode(obj_);
	}

	/** @brief Returns `true` if single-line mode is enabled. */
	bool is_one_line() const {
		return lv_textarea_get_one_line(obj_);
	}

	/** @brief Appends a single Unicode codepoint to the text. */
	Textarea &add_char(uint32_t c) {
		lv_textarea_add_char(obj_, c);
		return *this;
	}

	/** @brief Deletes the character before the cursor. */
	Textarea &delete_char() {
		lv_textarea_delete_char(obj_);
		return *this;
	}
};

static_assert(sizeof(Textarea) == sizeof(void *),
	      "Textarea must be pointer-sized");

/**
 * @class Dropdown
 * @brief C++ wrapper for an LVGL click-to-open dropdown list.
 *
 * Options are supplied as a single `\n`-separated string. The selected
 * index changes via `LV_EVENT_VALUE_CHANGED`.
 */
class Dropdown : public ObjectView,
		 public ObjectMixin<Dropdown>,
		 public EventMixin<Dropdown>,
		 public StyleMixin<Dropdown> {
public:
	explicit Dropdown(lv_obj_t *obj) : ObjectView(obj) {}

	static Dropdown create(ObjectView parent) {
		return Dropdown(lv_dropdown_create(parent));
	}

	/** @brief Sets options from a `\n`-separated string (LVGL copies it). */
	Dropdown &options(const char *opts) {
		lv_dropdown_set_options(obj_, opts);
		return *this;
	}

	/** @brief Sets options from a persistent `\n`-separated string (no copy). */
	Dropdown &options_static(const char *opts) {
		lv_dropdown_set_options_static(obj_, opts);
		return *this;
	}

	/** @brief Inserts a single option at `pos`. */
	Dropdown &add_option(const char *opt, uint32_t pos) {
		lv_dropdown_add_option(obj_, opt, pos);
		return *this;
	}

	/** @brief Clears the entire option list. */
	Dropdown &clear_options() {
		lv_dropdown_clear_options(obj_);
		return *this;
	}

	/** @brief Sets the currently selected option index. */
	Dropdown &selected(uint32_t sel) {
		lv_dropdown_set_selected(obj_, sel);
		return *this;
	}

	/** @brief Sets the popup direction. */
	Dropdown &dir(lv_dir_t d) {
		lv_dropdown_set_dir(obj_, d);
		return *this;
	}

	/** @brief Sets the dropdown-arrow icon (image dsc or symbol string). */
	Dropdown &symbol(const void *sym) {
		lv_dropdown_set_symbol(obj_, sym);
		return *this;
	}

	/** @brief Returns the currently selected option index. */
	uint32_t get_selected() const {
		return lv_dropdown_get_selected(obj_);
	}

	/** @brief Returns the total option count. */
	uint32_t get_option_count() const {
		return lv_dropdown_get_option_count(obj_);
	}

	/** @brief Fills `buf` with the currently selected option text. */
	void get_selected_str(char *buf, uint32_t size) const {
		lv_dropdown_get_selected_str(obj_, buf, size);
	}

	/** @brief Returns the entire option list as a `\n`-separated string. */
	const char *get_options() const {
		return lv_dropdown_get_options(obj_);
	}

	/** @brief Returns `true` if the popup list is currently expanded. */
	bool is_open() const {
		return lv_dropdown_is_open(const_cast<lv_obj_t *>(obj_));
	}

	/** @brief Expands the popup list. */
	void open() { lv_dropdown_open(obj_); }

	/** @brief Collapses the popup list. */
	void close() { lv_dropdown_close(obj_); }
};

static_assert(sizeof(Dropdown) == sizeof(void *),
	      "Dropdown must be pointer-sized");

/**
 * @class Roller
 * @brief C++ wrapper for LVGL's iOS-picker-style scrollable option wheel.
 *
 * Configured similarly to `Dropdown` (newline-separated options). The
 * `NORMAL` mode wraps at the ends; `INFINITE` allows wrapping through
 * the list multiple times.
 */
class Roller : public ObjectView,
	       public ObjectMixin<Roller>,
	       public EventMixin<Roller>,
	       public StyleMixin<Roller> {
public:
	explicit Roller(lv_obj_t *obj) : ObjectView(obj) {}

	static Roller create(ObjectView parent) {
		return Roller(lv_roller_create(parent));
	}

	/** @brief Sets options (newline-separated) and scroll mode. */
	Roller &options(const char *opts,
			lv_roller_mode_t mode = LV_ROLLER_MODE_NORMAL) {
		lv_roller_set_options(obj_, opts, mode);
		return *this;
	}

	/** @brief Sets the currently selected option index. */
	Roller &selected(uint32_t sel,
			 lv_anim_enable_t anim = LV_ANIM_OFF) {
		lv_roller_set_selected(obj_, sel, anim);
		return *this;
	}

	/** @brief Sets how many rows are visible in the wheel at once. */
	Roller &visible_row_count(uint32_t rows) {
		lv_roller_set_visible_row_count(obj_, rows);
		return *this;
	}

	/** @brief Returns the currently selected option index. */
	uint32_t get_selected() const {
		return lv_roller_get_selected(obj_);
	}

	/** @brief Returns the total option count. */
	uint32_t get_option_count() const {
		return lv_roller_get_option_count(obj_);
	}

	/** @brief Fills `buf` with the currently selected option text. */
	void get_selected_str(char *buf, uint32_t size) const {
		lv_roller_get_selected_str(obj_, buf, size);
	}

	/** @brief Returns the entire option list as a `\n`-separated string. */
	const char *get_options() const {
		return lv_roller_get_options(obj_);
	}
};

static_assert(sizeof(Roller) == sizeof(void *),
	      "Roller must be pointer-sized");

/**
 * @class Spinbox
 * @brief C++ wrapper for an LVGL numeric stepper (`+` / `−` buttons).
 *
 * `digit_format(d, s)` controls the display layout — `d` is the total
 * digit count, `s` is the (1-based) decimal-point position from the
 * right. Example: `digit_format(5, 2)` renders `###.##`.
 */
class Spinbox : public ObjectView,
		public ObjectMixin<Spinbox>,
		public EventMixin<Spinbox>,
		public StyleMixin<Spinbox> {
public:
	explicit Spinbox(lv_obj_t *obj) : ObjectView(obj) {}

	static Spinbox create(ObjectView parent) {
		return Spinbox(lv_spinbox_create(parent));
	}

	/** @brief Sets the current integer value (scaled by digit_format). */
	Spinbox &value(int32_t v) {
		lv_spinbox_set_value(obj_, v);
		return *this;
	}

	/** @brief Sets min/max bounds. */
	Spinbox &range(int32_t min, int32_t max) {
		lv_spinbox_set_range(obj_, min, max);
		return *this;
	}

	/** @brief Sets the increment/decrement step size. */
	Spinbox &step(uint32_t s) {
		lv_spinbox_set_step(obj_, s);
		return *this;
	}

	/**
	 * @brief Configures how the numeric value is displayed.
	 * @param[in] digit_count Total digit count including decimals.
	 * @param[in] sep_pos     Decimal-point position (1 = right-most).
	 */
	Spinbox &digit_format(uint32_t digit_count, uint32_t sep_pos) {
		lv_spinbox_set_digit_format(obj_, digit_count, sep_pos);
		return *this;
	}

	/** @brief Enables wrap-around at min/max bounds. */
	Spinbox &rollover(bool on) {
		lv_spinbox_set_rollover(obj_, on);
		return *this;
	}

	/** @brief Sets the cursor to edit a specific digit. */
	Spinbox &cursor_pos(uint32_t pos) {
		lv_spinbox_set_cursor_pos(obj_, pos);
		return *this;
	}

	/** @brief Returns the current integer value. */
	int32_t get_value() const {
		return lv_spinbox_get_value(const_cast<lv_obj_t *>(obj_));
	}

	/** @brief Returns the current step size. */
	int32_t get_step() const {
		return lv_spinbox_get_step(const_cast<lv_obj_t *>(obj_));
	}

	/** @brief Increments the value by one step. */
	Spinbox &increment() {
		lv_spinbox_increment(obj_);
		return *this;
	}

	/** @brief Decrements the value by one step. */
	Spinbox &decrement() {
		lv_spinbox_decrement(obj_);
		return *this;
	}
};

static_assert(sizeof(Spinbox) == sizeof(void *),
	      "Spinbox must be pointer-sized");

/**
 * @class Keyboard
 * @brief C++ wrapper for LVGL's on-screen keyboard.
 *
 * `attach(textarea)` binds this keyboard so typing inserts characters
 * into the target `Textarea`. `mode()` switches between lowercase,
 * uppercase, special characters, and number-pad layouts.
 */
class Keyboard : public ObjectView,
		 public ObjectMixin<Keyboard>,
		 public EventMixin<Keyboard>,
		 public StyleMixin<Keyboard> {
public:
	explicit Keyboard(lv_obj_t *obj) : ObjectView(obj) {}

	static Keyboard create(ObjectView parent) {
		return Keyboard(lv_keyboard_create(parent));
	}

	/** @brief Attaches to a Textarea so typed keys flow to it. */
	Keyboard &attach(Textarea ta) {
		lv_keyboard_set_textarea(obj_, ta.get());
		return *this;
	}

	/** @brief Switches keyboard mode (lowercase/upper/special/number). */
	Keyboard &mode(lv_keyboard_mode_t m) {
		lv_keyboard_set_mode(obj_, m);
		return *this;
	}

	/** @brief Enables or disables on-press popovers for letter keys. */
	Keyboard &popovers(bool en) {
		lv_keyboard_set_popovers(obj_, en);
		return *this;
	}

	/** @brief Returns the currently attached Textarea, or a null view. */
	ObjectView get_textarea() const {
		return ObjectView(lv_keyboard_get_textarea(obj_));
	}
};

static_assert(sizeof(Keyboard) == sizeof(void *),
	      "Keyboard must be pointer-sized");

/**
 * @class Chart
 * @brief C++ wrapper for an LVGL chart (line / bar / scatter).
 *
 * A chart owns a fixed-capacity buffer of points per series. Use
 * `add_series(color, axis)` to create a data line, then push values via
 * `Series::next_value(v)` for shift-and-append updates or
 * `Series::set_value_by_idx(i, v)` for random access. `update_mode()`
 * controls whether `next_value` shifts existing points left (SHIFT) or
 * wraps around (CIRCULAR).
 */
class Chart : public ObjectView,
	      public ObjectMixin<Chart>,
	      public EventMixin<Chart>,
	      public StyleMixin<Chart> {
public:
	/** @brief Lightweight handle to an `lv_chart_series_t *`. */
	class Series {
	public:
		Series() : chart_(nullptr), ser_(nullptr) {}
		Series(lv_obj_t *chart, lv_chart_series_t *ser)
			: chart_(chart), ser_(ser) {}

		lv_chart_series_t *get() const { return ser_; }
		explicit operator bool() const { return ser_ != nullptr; }

		/** @brief Pushes a new value using the current update mode. */
		Series &next_value(int32_t v) {
			lv_chart_set_next_value(chart_, ser_, v);
			return *this;
		}

		/** @brief Sets a specific point index to `v`. */
		Series &set_value_by_idx(uint32_t idx, int32_t v) {
			lv_chart_set_value_by_id(chart_, ser_, idx, v);
			return *this;
		}

	private:
		lv_obj_t *chart_;
		lv_chart_series_t *ser_;
	};

	explicit Chart(lv_obj_t *obj) : ObjectView(obj) {}

	static Chart create(ObjectView parent) {
		return Chart(lv_chart_create(parent));
	}

	/** @brief Sets the chart type (line / bar / scatter / none). */
	Chart &type(lv_chart_type_t t) {
		lv_chart_set_type(obj_, t);
		return *this;
	}

	/** @brief Sets the number of data points per series. */
	Chart &point_count(uint32_t count) {
		lv_chart_set_point_count(obj_, count);
		return *this;
	}

	/** @brief Sets the min/max range for the given axis. */
	Chart &range(lv_chart_axis_t axis, int32_t min, int32_t max) {
		lv_chart_set_range(obj_, axis, min, max);
		return *this;
	}

	/** @brief Sets how `next_value()` updates the point array. */
	Chart &update_mode(lv_chart_update_mode_t mode) {
		lv_chart_set_update_mode(obj_, mode);
		return *this;
	}

	/** @brief Sets the horizontal/vertical division line count. */
	Chart &div_line_count(uint8_t hdiv, uint8_t vdiv) {
		lv_chart_set_div_line_count(obj_, hdiv, vdiv);
		return *this;
	}

	/** @brief Adds a new series with the given colour and axis binding. */
	Series add_series(lv_color_t color, lv_chart_axis_t axis) {
		lv_chart_series_t *s = lv_chart_add_series(obj_, color, axis);
		return Series(obj_, s);
	}

	/** @brief Removes a series from the chart. */
	Chart &remove_series(Series s) {
		lv_chart_remove_series(obj_, s.get());
		return *this;
	}
};

static_assert(sizeof(Chart) == sizeof(void *),
	      "Chart must be pointer-sized");

/**
 * @class Table
 * @brief C++ wrapper for LVGL's 2D data table widget.
 *
 * Cells are addressed by `(row, col)` and store a text string copied
 * into LVGL-owned memory. Use `cell_value_fmt` for printf-style updates.
 */
class Table : public ObjectView,
	      public ObjectMixin<Table>,
	      public EventMixin<Table>,
	      public StyleMixin<Table> {
public:
	explicit Table(lv_obj_t *obj) : ObjectView(obj) {}

	static Table create(ObjectView parent) {
		return Table(lv_table_create(parent));
	}

	/** @brief Sets a cell's text (LVGL copies the string). */
	Table &cell_value(uint32_t row, uint32_t col, const char *txt) {
		lv_table_set_cell_value(obj_, row, col, txt);
		return *this;
	}

	/** @brief Sets the row count (grows/shrinks the table). */
	Table &row_count(uint32_t n) {
		lv_table_set_row_count(obj_, n);
		return *this;
	}

	/** @brief Sets the column count. */
	Table &column_count(uint32_t n) {
		lv_table_set_column_count(obj_, n);
		return *this;
	}

	/** @brief Sets the width of a specific column in pixels. */
	Table &column_width(uint32_t col, int32_t w) {
		lv_table_set_column_width(obj_, col, w);
		return *this;
	}

	/** @brief Returns the text at `(row, col)`. */
	const char *get_cell_value(uint32_t row, uint32_t col) const {
		return lv_table_get_cell_value(obj_, row, col);
	}

	/** @brief Returns the total row count. */
	uint32_t get_row_count() const {
		return lv_table_get_row_count(obj_);
	}

	/** @brief Returns the total column count. */
	uint32_t get_column_count() const {
		return lv_table_get_column_count(obj_);
	}

	/** @brief Returns the width of a specific column in pixels. */
	int32_t get_column_width(uint32_t col) const {
		return lv_table_get_column_width(obj_, col);
	}
};

static_assert(sizeof(Table) == sizeof(void *),
	      "Table must be pointer-sized");

/**
 * @class Tabview
 * @brief C++ wrapper for LVGL's tabbed container with swipeable pages.
 *
 * Call `add_tab(name)` to create a tab; the returned `ObjectView` is
 * the tab's content container — add children directly to it.
 */
class Tabview : public ObjectView,
		public ObjectMixin<Tabview>,
		public EventMixin<Tabview>,
		public StyleMixin<Tabview> {
public:
	explicit Tabview(lv_obj_t *obj) : ObjectView(obj) {}

	/**
	 * @brief Creates a tabview with the given tab-bar position and size.
	 * @param[in] parent    Parent object (or null for screen).
	 * @param[in] tab_pos   `LV_DIR_TOP/BOTTOM/LEFT/RIGHT`.
	 * @param[in] tab_size  Tab bar height (top/bottom) or width (left/right) in pixels.
	 */
	static Tabview create(ObjectView parent) {
		return Tabview(lv_tabview_create(parent));
	}

	/** @brief Adds a new tab and returns its content container. */
	ObjectView add_tab(const char *name) {
		return ObjectView(lv_tabview_add_tab(obj_, name));
	}

	/** @brief Renames an existing tab. */
	Tabview &rename_tab(uint32_t idx, const char *name) {
		lv_tabview_rename_tab(obj_, idx, name);
		return *this;
	}

	/** @brief Sets the active tab index with optional animation. */
	Tabview &set_active_tab(uint32_t idx, lv_anim_enable_t anim) {
		lv_tabview_set_active(obj_, idx, anim);
		return *this;
	}

	/** @brief Sets the tab bar position (TOP/BOTTOM/LEFT/RIGHT). */
	Tabview &tab_bar_position(lv_dir_t dir) {
		lv_tabview_set_tab_bar_position(obj_, dir);
		return *this;
	}

	/** @brief Sets the tab bar size in pixels. */
	Tabview &tab_bar_size(int32_t size) {
		lv_tabview_set_tab_bar_size(obj_, size);
		return *this;
	}

	/** @brief Returns the number of tabs. */
	uint32_t get_tab_count() const {
		return lv_tabview_get_tab_count(obj_);
	}

	/** @brief Returns the currently active tab index. */
	uint32_t get_tab_active() const {
		return lv_tabview_get_tab_active(obj_);
	}

	/** @brief Returns the content container (all tabs live here). */
	ObjectView get_content() const {
		return ObjectView(lv_tabview_get_content(obj_));
	}
};

static_assert(sizeof(Tabview) == sizeof(void *),
	      "Tabview must be pointer-sized");

/**
 * @class List
 * @brief C++ wrapper for LVGL's scrollable list of buttons/text.
 *
 * A List is essentially a vertical-flex Box that adds helpers for
 * inserting text rows and icon-plus-label button rows.
 */
class List : public ObjectView,
	     public ObjectMixin<List>,
	     public EventMixin<List>,
	     public StyleMixin<List> {
public:
	explicit List(lv_obj_t *obj) : ObjectView(obj) {}

	static List create(ObjectView parent) {
		return List(lv_list_create(parent));
	}

	/** @brief Adds a text heading row. Returns the label object. */
	ObjectView add_text(const char *text) {
		return ObjectView(lv_list_add_text(obj_, text));
	}

	/** @brief Adds a button row with icon and text. Returns the button. */
	ObjectView add_button(const void *icon, const char *text) {
		return ObjectView(lv_list_add_button(obj_, icon, text));
	}

	/** @brief Returns the text of a list button. */
	const char *get_button_text(ObjectView btn) const {
		return lv_list_get_button_text(obj_, btn);
	}
};

static_assert(sizeof(List) == sizeof(void *),
	      "List must be pointer-sized");

/**
 * @class Canvas
 * @brief C++ wrapper for LVGL's user-buffer drawing surface.
 *
 * The canvas owns a pixel buffer the caller supplies. LVGL draws rects,
 * arcs, text, etc. into that buffer via the `draw_*` helper functions
 * below. The buffer must outlive the canvas widget; size is
 * `w * h * bytes_per_pixel(cf)`.
 *
 * This initial cut exposes rect, arc, and label draw primitives. Line,
 * polygon, and image draws can be added in follow-up work as they
 * require wrapping more draw-descriptor structs.
 */
class Canvas : public ObjectView,
	       public ObjectMixin<Canvas>,
	       public EventMixin<Canvas>,
	       public StyleMixin<Canvas> {
public:
	explicit Canvas(lv_obj_t *obj) : ObjectView(obj) {}

	static Canvas create(ObjectView parent) {
		return Canvas(lv_canvas_create(parent));
	}

	/** @brief Attaches a pixel buffer of the given geometry and format. */
	Canvas &buffer(void *buf, int32_t w, int32_t h, lv_color_format_t cf) {
		lv_canvas_set_buffer(obj_, buf, w, h, cf);
		return *this;
	}

	/** @brief Fills the entire canvas with a solid color + opacity. */
	Canvas &fill_bg(lv_color_t color, lv_opa_t opa) {
		lv_canvas_fill_bg(obj_, color, opa);
		return *this;
	}

	/** @brief Sets a single pixel to the given colour. */
	Canvas &set_pixel(int32_t x, int32_t y, lv_color_t color) {
		lv_canvas_set_px(obj_, x, y, color, LV_OPA_COVER);
		return *this;
	}

	/**
	 * @brief Begins a draw sequence on the canvas layer.
	 *
	 * Must be called before any `lv_draw_*` calls; pair with
	 * `finish_layer()`. LVGL 9 requires this layer dance for every
	 * draw operation on a canvas.
	 */
	Canvas &init_layer(lv_layer_t *layer) {
		lv_canvas_init_layer(obj_, layer);
		return *this;
	}

	/** @brief Finishes a draw sequence started by `init_layer()`. */
	Canvas &finish_layer(lv_layer_t *layer) {
		lv_canvas_finish_layer(obj_, layer);
		return *this;
	}
};

static_assert(sizeof(Canvas) == sizeof(void *),
	      "Canvas must be pointer-sized");

/**
 * @class Calendar
 * @brief C++ wrapper for LVGL's month-grid date picker.
 *
 * Configure `today(y, m, d)` and `showed(y, m)` to set the highlighted
 * "today" marker and the currently visible month. `highlighted_dates`
 * marks additional dates with a visual accent. Use `get_pressed_date()`
 * from an `on_click` handler to read which cell the user selected.
 */
class Calendar : public ObjectView,
		 public ObjectMixin<Calendar>,
		 public EventMixin<Calendar>,
		 public StyleMixin<Calendar> {
public:
	explicit Calendar(lv_obj_t *obj) : ObjectView(obj) {}

	static Calendar create(ObjectView parent) {
		return Calendar(lv_calendar_create(parent));
	}

	/** @brief Sets today's date (highlighted in the grid). */
	Calendar &today(uint32_t year, uint32_t month, uint32_t day) {
		lv_calendar_set_today_date(obj_, year, month, day);
		return *this;
	}

	/** @brief Sets the currently visible month (year/month). */
	Calendar &showed(uint32_t year, uint32_t month) {
		lv_calendar_set_showed_date(obj_, year, month);
		return *this;
	}

	/** @brief Highlights a list of dates with a visual accent. LVGL
	 *         retains the pointer — the array must outlive the calendar. */
	Calendar &highlighted_dates(lv_calendar_date_t *dates, size_t count) {
		lv_calendar_set_highlighted_dates(obj_, dates,
						  static_cast<uint16_t>(count));
		return *this;
	}

	/** @brief Returns the last date the user pressed in the grid. */
	bool get_pressed_date(lv_calendar_date_t *out) const {
		return lv_calendar_get_pressed_date(obj_, out);
	}

	/** @brief Adds an arrow-header navigation bar as a child. */
	ObjectView add_header_arrow() {
		return ObjectView(lv_calendar_header_arrow_create(obj_));
	}

	/** @brief Adds a dropdown-header navigation bar as a child. */
	ObjectView add_header_dropdown() {
		return ObjectView(lv_calendar_header_dropdown_create(obj_));
	}
};

static_assert(sizeof(Calendar) == sizeof(void *),
	      "Calendar must be pointer-sized");

/* ================================================================== */
/*  Layout helpers                                                    */
/* ================================================================== */

/**
 * @brief Creates a vertical flex container (`LV_FLEX_FLOW_COLUMN`) sized to its content.
 * @param[in] parent The parent `ObjectView`.
 * @return A `Box` configured as a vertical flex container.
 */
inline Box vbox(ObjectView parent) {
	auto b = Box::create(parent);
	lv_obj_set_flex_flow(b.get(), LV_FLEX_FLOW_COLUMN);
	lv_obj_set_size(b.get(), LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	return b;
}

/**
 * @brief Creates a horizontal flex container (`LV_FLEX_FLOW_ROW`) sized to its content.
 * @param[in] parent The parent `ObjectView`.
 * @return A `Box` configured as a horizontal flex container.
 */
inline Box hbox(ObjectView parent) {
	auto b = Box::create(parent);
	lv_obj_set_flex_flow(b.get(), LV_FLEX_FLOW_ROW);
	lv_obj_set_size(b.get(), LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	return b;
}

/* ================================================================== */
/*  Group — keyboard / encoder focus groups                           */
/* ================================================================== */

/**
 * @class Group
 * @brief RAII wrapper for an LVGL input focus group (`lv_group_t`).
 *
 * Groups route keyboard / encoder / button events to a focused member
 * widget. Essential for non-touch targets where pointer input isn't
 * available. Create a group, add widgets with `add()`, then connect
 * your input device to the group (outside the scope of this wrapper —
 * see backend-specific input device registration).
 *
 * @note Non-copyable, movable. Owns the underlying `lv_group_t *`.
 */
class Group {
public:
	Group() : group_(nullptr) {}
	explicit Group(lv_group_t *g) : group_(g) {}

	~Group() { if (group_) lv_group_delete(group_); }

	Group(const Group &) = delete;
	Group &operator=(const Group &) = delete;

	Group(Group &&other) noexcept : group_(other.group_) {
		other.group_ = nullptr;
	}

	Group &operator=(Group &&other) noexcept {
		if (this != &other) {
			if (group_) lv_group_delete(group_);
			group_ = other.group_;
			other.group_ = nullptr;
		}
		return *this;
	}

	/** @brief Creates a fresh focus group. */
	static Group create() { return Group(lv_group_create()); }

	/** @brief Returns the default group (set via `set_as_default()`). */
	static lv_group_t *get_default() { return lv_group_get_default(); }

	/** @brief Raw `lv_group_t *` for FFI interop. */
	lv_group_t *get() const { return group_; }

	/** @brief Marks this group as the default — new focusable widgets
	 *         will auto-join it at creation time. */
	Group &set_as_default() {
		lv_group_set_default(group_);
		return *this;
	}

	/** @brief Adds a widget to the group's focusable list. */
	Group &add(ObjectView obj) {
		lv_group_add_obj(group_, obj);
		return *this;
	}

	/** @brief Removes all widgets from this group. */
	Group &remove_all() {
		lv_group_remove_all_objs(group_);
		return *this;
	}

	/** @brief Moves focus to the next member. */
	Group &focus_next() {
		lv_group_focus_next(group_);
		return *this;
	}

	/** @brief Moves focus to the previous member. */
	Group &focus_prev() {
		lv_group_focus_prev(group_);
		return *this;
	}

	/** @brief Freezes/unfreezes focus movement. */
	Group &focus_freeze(bool freeze) {
		lv_group_focus_freeze(group_, freeze);
		return *this;
	}

	/** @brief Returns the currently focused widget, or a null view. */
	ObjectView focused() const {
		return ObjectView(lv_group_get_focused(group_));
	}

	/** @brief Enables/disables edit mode (relevant for encoder input). */
	Group &edit_mode(bool enable) {
		lv_group_set_editing(group_, enable);
		return *this;
	}

	/** @brief Returns the current edit-mode flag. */
	bool is_editing() const {
		return lv_group_get_editing(group_);
	}

	/** @brief Returns the number of widgets in the group. */
	uint32_t obj_count() const {
		return lv_group_get_obj_count(group_);
	}

	/** @brief Static helper: focus a specific widget regardless of its
	 *         group membership. */
	static void focus_obj(ObjectView obj) {
		lv_group_focus_obj(obj);
	}

	/** @brief Static helper: remove a widget from whatever group it's in. */
	static void remove_obj(ObjectView obj) {
		lv_group_remove_obj(obj);
	}

private:
	lv_group_t *group_;
};

/* ================================================================== */
/*  Screen — top-level display root with load/load_anim               */
/* ================================================================== */

/**
 * @class Screen
 * @brief Top-level LVGL screen — a display root without a parent.
 *
 * Screens are plain `lv_obj_t` instances created with `nullptr` parent.
 * Swap between screens via `load()` or `load_anim()` to drive multi-page
 * navigation. All layout/style/event APIs inherited from the mixins work
 * the same as on a Box.
 *
 * @note Does not own the underlying LVGL object. Use `del()` to free it
 *       (or let `load_anim(..., auto_del=true)` handle it during the
 *       transition).
 */
class Screen : public ObjectView,
	       public ObjectMixin<Screen>,
	       public EventMixin<Screen>,
	       public StyleMixin<Screen> {
public:
	explicit Screen(lv_obj_t *obj) : ObjectView(obj) {}

	/** @brief Creates a new top-level screen with no parent. */
	static Screen create() {
		return Screen(lv_obj_create(nullptr));
	}

	/** @brief Returns the currently active screen. */
	static Screen active() {
		return Screen(lv_screen_active());
	}

	/** @brief Instantly loads this screen as the active one. */
	void load() { lv_screen_load(obj_); }

	/**
	 * @brief Loads this screen with an animated transition.
	 * @param[in] anim     Animation type (`LV_SCR_LOAD_ANIM_*`).
	 * @param[in] time_ms  Duration in milliseconds.
	 * @param[in] delay_ms Delay before the transition starts.
	 * @param[in] auto_del When true, deletes the previously active screen
	 *                     after the transition completes. Any surviving
	 *                     references to the old screen become invalid.
	 */
	void load_anim(lv_screen_load_anim_t anim, uint32_t time_ms,
		       uint32_t delay_ms = 0, bool auto_del = false) {
		lv_screen_load_anim(obj_, anim, time_ms, delay_ms, auto_del);
	}
};

static_assert(sizeof(Screen) == sizeof(void *),
	      "Screen must be pointer-sized");

/* ================================================================== */
/*  Timer — RAII wrapper around lv_timer_t                            */
/* ================================================================== */

/**
 * @class Timer
 * @brief RAII wrapper for LVGL's built-in timer system.
 *
 * LVGL timers fire from inside `lv_timer_handler()` under the existing
 * LVGL lock — callbacks do NOT need to acquire `LvglGuard` again. This is
 * the preferred timer for UI updates driven by the LVGL task itself,
 * versus `ove::Timer` which fires from an OS timer task and requires
 * manual locking in the callback.
 *
 * @warning Do NOT call `lvgl::lock()` / construct `LvglGuard` from inside
 *          a timer callback — the LVGL mutex is not reentrant.
 *
 * @note Non-copyable, movable. Owns the underlying `lv_timer_t *`.
 */
class Timer {
public:
	/** @brief Constructs a null (inactive) timer. */
	Timer() : timer_(nullptr) {}

	/**
	 * @brief Creates and starts an LVGL timer.
	 * @param[in] cb        Callback invoked every `period_ms` milliseconds.
	 * @param[in] period_ms Timer period in milliseconds.
	 * @param[in] user_data Opaque pointer retrievable via `lv_timer_get_user_data()`.
	 */
	Timer(lv_timer_cb_t cb, uint32_t period_ms, void *user_data = nullptr)
		: timer_(lv_timer_create(cb, period_ms, user_data)) {}

	~Timer() { if (timer_) lv_timer_delete(timer_); }

	Timer(const Timer &) = delete;
	Timer &operator=(const Timer &) = delete;

	Timer(Timer &&other) noexcept : timer_(other.timer_) {
		other.timer_ = nullptr;
	}

	Timer &operator=(Timer &&other) noexcept {
		if (this != &other) {
			if (timer_) lv_timer_delete(timer_);
			timer_ = other.timer_;
			other.timer_ = nullptr;
		}
		return *this;
	}

	/** @brief Returns the raw `lv_timer_t *` for FFI interop. */
	lv_timer_t *get() const { return timer_; }

	/** @brief `true` when this Timer holds a live LVGL timer. */
	explicit operator bool() const { return timer_ != nullptr; }

	/** @brief Updates the timer period in milliseconds. */
	Timer &period(uint32_t ms) {
		if (timer_) lv_timer_set_period(timer_, ms);
		return *this;
	}

	/** @brief Pauses the timer (can be resumed). */
	Timer &pause() {
		if (timer_) lv_timer_pause(timer_);
		return *this;
	}

	/** @brief Resumes a paused timer. */
	Timer &resume() {
		if (timer_) lv_timer_resume(timer_);
		return *this;
	}

	/**
	 * @brief Sets the number of times the timer should fire.
	 * @param[in] count Remaining fires, or `-1` for infinite.
	 */
	Timer &repeat_count(int32_t count) {
		if (timer_) lv_timer_set_repeat_count(timer_, count);
		return *this;
	}

	/** @brief Resets the internal elapsed-time counter. */
	Timer &reset() {
		if (timer_) lv_timer_reset(timer_);
		return *this;
	}

	/** @brief Makes the timer ready to fire on the next `lv_timer_handler()` pass. */
	Timer &ready() {
		if (timer_) lv_timer_ready(timer_);
		return *this;
	}

private:
	lv_timer_t *timer_;
};

/* ================================================================== */
/*  Animation — builder for lv_anim_t                                 */
/* ================================================================== */

/**
 * @class Animation
 * @brief Fluent builder for LVGL animations (`lv_anim_t`).
 *
 * Configure an animation step by step, then call `start()` — LVGL copies
 * the state into its internal animation list at that moment, so the
 * `Animation` object can be destructed immediately after.
 *
 * Stateless lambdas and plain function pointers work directly as exec
 * and ready callbacks. For the most common cases see the `animate_x`,
 * `animate_y`, `animate_width`, `animate_opa` helpers at namespace scope.
 *
 * @note Callbacks run from the LVGL task context while the LVGL lock is
 *       already held; do NOT re-acquire the lock inside them.
 */
class Animation {
public:
	Animation() { lv_anim_init(&anim_); }

	/** @brief Sets the target variable pointer (typically an `lv_obj_t *`). */
	Animation &target(void *var) {
		lv_anim_set_var(&anim_, var);
		return *this;
	}

	/** @brief Sets the target as an LVGL object. */
	Animation &target(ObjectView obj) {
		return target(obj.get());
	}

	/** @brief Sets the start and end values. */
	Animation &values(int32_t from, int32_t to) {
		lv_anim_set_values(&anim_, from, to);
		return *this;
	}

	/** @brief Sets the animation duration in milliseconds. */
	Animation &duration(uint32_t ms) {
		lv_anim_set_duration(&anim_, ms);
		return *this;
	}

	/** @brief Sets the delay before the animation starts, in milliseconds. */
	Animation &delay(uint32_t ms) {
		lv_anim_set_delay(&anim_, ms);
		return *this;
	}

	/** @brief Sets the easing curve. Use `lv_anim_path_linear`, `_ease_out`, etc. */
	Animation &path(lv_anim_path_cb_t cb) {
		lv_anim_set_path_cb(&anim_, cb);
		return *this;
	}

	/** @brief Sets the repeat count; use `LV_ANIM_REPEAT_INFINITE` for endless. */
	Animation &repeat_count(uint32_t count) {
		lv_anim_set_repeat_count(&anim_, count);
		return *this;
	}

	/** @brief Sets the delay between repeats. */
	Animation &repeat_delay(uint32_t ms) {
		lv_anim_set_repeat_delay(&anim_, ms);
		return *this;
	}

	/** @brief Sets the duration of the playback (reverse) phase. */
	Animation &playback_duration(uint32_t ms) {
		lv_anim_set_playback_duration(&anim_, ms);
		return *this;
	}

	/** @brief Sets the delay before the playback phase. */
	Animation &playback_delay(uint32_t ms) {
		lv_anim_set_playback_delay(&anim_, ms);
		return *this;
	}

	/**
	 * @brief Sets the exec callback invoked on every frame.
	 * @param[in] cb Function taking `(void *var, int32_t value)`.
	 */
	Animation &exec_cb(lv_anim_exec_xcb_t cb) {
		lv_anim_set_exec_cb(&anim_, cb);
		return *this;
	}

	/** @brief Sets the ready callback invoked when the animation finishes. */
	Animation &ready_cb(lv_anim_ready_cb_t cb) {
		lv_anim_set_ready_cb(&anim_, cb);
		return *this;
	}

	/**
	 * @brief Starts the animation. LVGL copies the state into its
	 *        internal list, so this `Animation` can be destructed after.
	 */
	void start() { lv_anim_start(&anim_); }

	/** @brief Stops any animations matching `(var, exec_cb)`. */
	static bool stop(void *var, lv_anim_exec_xcb_t exec_cb) {
		return lv_anim_delete(var, exec_cb);
	}

private:
	lv_anim_t anim_;
};

/** @brief Helper: animate an object's X position with ease-out. */
inline void animate_x(ObjectView obj, int32_t to, uint32_t duration_ms) {
	Animation()
		.target(obj)
		.values(lv_obj_get_x(obj), to)
		.duration(duration_ms)
		.path(lv_anim_path_ease_out)
		.exec_cb(reinterpret_cast<lv_anim_exec_xcb_t>(lv_obj_set_x))
		.start();
}

/** @brief Helper: animate an object's Y position with ease-out. */
inline void animate_y(ObjectView obj, int32_t to, uint32_t duration_ms) {
	Animation()
		.target(obj)
		.values(lv_obj_get_y(obj), to)
		.duration(duration_ms)
		.path(lv_anim_path_ease_out)
		.exec_cb(reinterpret_cast<lv_anim_exec_xcb_t>(lv_obj_set_y))
		.start();
}

/** @brief Helper: animate an object's width. */
inline void animate_width(ObjectView obj, int32_t to, uint32_t duration_ms) {
	Animation()
		.target(obj)
		.values(lv_obj_get_width(obj), to)
		.duration(duration_ms)
		.path(lv_anim_path_ease_out)
		.exec_cb(reinterpret_cast<lv_anim_exec_xcb_t>(lv_obj_set_width))
		.start();
}

namespace detail {
inline void anim_set_opa_shim(void *var, int32_t v) {
	lv_obj_set_style_opa(static_cast<lv_obj_t *>(var),
			     static_cast<lv_opa_t>(v), LV_PART_MAIN);
}
} // namespace detail

/** @brief Helper: fade an object's main-part opacity from current → `to`. */
inline void animate_opa(ObjectView obj, lv_opa_t to, uint32_t duration_ms) {
	lv_opa_t from = lv_obj_get_style_opa(obj, LV_PART_MAIN);
	Animation()
		.target(obj)
		.values(from, to)
		.duration(duration_ms)
		.path(lv_anim_path_ease_in_out)
		.exec_cb(detail::anim_set_opa_shim)
		.start();
}

/* ================================================================== */
/*  Component<Derived> — CRTP UI composition base                     */
/* ================================================================== */

/**
 * @class Component
 * @brief CRTP base class for reusable, self-contained LVGL UI components.
 *
 * A `Component` encapsulates a subtree of LVGL widgets behind a
 * `mount()` / `unmount()` lifecycle.  The derived class must implement a
 * `build(ObjectView parent)` method that constructs and returns the root
 * widget of the component.
 *
 * The root object's user-data pointer is set to `this` so that event
 * handlers can recover the component instance via `from_event()`.  An
 * internal `LV_EVENT_DELETE` callback clears the cached root reference if
 * the LVGL object tree is deleted externally (e.g., by a screen transition).
 *
 * @tparam Derived The concrete component class.  Must implement:
 *                 `ObjectView build(ObjectView parent)`.
 *
 * @note Non-copyable.
 * @note All methods must be called from within the LVGL task context or
 *       while holding an `LvglGuard` lock.
 */
template <typename Derived>
class Component {
public:
	/** @brief Default constructor — component starts in the unmounted state. */
	Component() = default;
	~Component() = default;

	Component(const Component &) = delete;
	Component &operator=(const Component &) = delete;

	/**
	 * @brief Builds and mounts the component under the given parent.
	 *
	 * Calls `Derived::build(parent)` to create the widget subtree, stores
	 * the component pointer in the root's user-data slot, and registers
	 * an internal `LV_EVENT_DELETE` callback.  If the component is already
	 * mounted, this call is a no-op.
	 *
	 * @param[in] parent The parent `ObjectView` to attach the component to.
	 */
	void mount(ObjectView parent) {
		if (root_)
			return;
		root_ = static_cast<Derived *>(this)->build(parent);
		/* Store 'this' in root's user data for from_event() */
		lv_obj_set_user_data(root_.get(), this);
		/* Track external deletion (e.g. screen auto_del) */
		lv_obj_add_event_cb(root_.get(),
				    delete_cb, LV_EVENT_DELETE, this);
	}

	/**
	 * @brief Unmounts and deletes the component's widget subtree.
	 *
	 * The internal `LV_EVENT_DELETE` callback is bypassed by clearing
	 * `root_` before calling `lv_obj_delete()`.  If the component is not
	 * mounted, this call is a no-op.
	 */
	void unmount() {
		if (!root_)
			return;
		/* Remove the delete callback before we delete, to avoid
		 * the callback nulling root_ and then us using it. */
		lv_obj_t *obj = root_.get();
		root_ = ObjectView();
		lv_obj_delete(obj);
	}

	/**
	 * @brief Returns `true` if the component is currently mounted.
	 * @return `true` when the root widget exists.
	 */
	bool is_mounted() const { return static_cast<bool>(root_); }

	/**
	 * @brief Returns an `ObjectView` of the component's root widget.
	 * @return The root widget, or a null `ObjectView` if not mounted.
	 */
	ObjectView root() const { return root_; }

	/**
	 * @brief Hides the component's root widget if mounted.
	 */
	void hide() {
		if (root_)
			lv_obj_add_flag(root_.get(), LV_OBJ_FLAG_HIDDEN);
	}

	/**
	 * @brief Shows the component's root widget if mounted.
	 */
	void show() {
		if (root_)
			lv_obj_remove_flag(root_.get(), LV_OBJ_FLAG_HIDDEN);
	}

	/**
	 * @brief Walks up the LVGL object tree from the event target to find
	 *        the `Derived` component instance.
	 *
	 * Retrieves the user-data pointer stored by `mount()` from each ancestor
	 * until a non-null pointer is found, then casts it to `Derived*`.
	 *
	 * @param[in] e The LVGL event whose target is the starting point of the walk.
	 * @return Pointer to the `Derived` component, or `nullptr` if not found.
	 */
	static Derived *from_event(lv_event_t *e) {
		lv_obj_t *target = static_cast<lv_obj_t *>(lv_event_get_target(e));
		while (target) {
			void *ud = lv_obj_get_user_data(target);
			if (ud)
				return static_cast<Derived *>(
					static_cast<Component *>(ud));
			target = lv_obj_get_parent(target);
		}
		return nullptr;
	}

protected:
	/** @brief Non-owning view of the component's root LVGL widget. */
	ObjectView root_;

private:
	/**
	 * @brief Internal LVGL event callback that clears `root_` when the
	 *        underlying object is deleted externally.
	 * @param[in] e The `LV_EVENT_DELETE` event.
	 */
	static void delete_cb(lv_event_t *e) {
		auto *self = static_cast<Component *>(
			lv_event_get_user_data(e));
		/* Object is being deleted externally — just clear our ref */
		self->root_ = ObjectView();
	}
};

} /* namespace ove::lvgl */
