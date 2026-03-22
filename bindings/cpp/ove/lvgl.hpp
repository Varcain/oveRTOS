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
	 * @brief Sets the background opacity.
	 * @param[in] opa Opacity value (0–255, or `LV_OPA_*` constants).
	 * @return Reference to the derived object for method chaining.
	 */
	Derived &bg_opa(lv_opa_t opa) {
		lv_obj_set_style_bg_opa(self().get(), opa, LV_PART_MAIN);
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

	/**
	 * @brief Sets the corner radius.
	 * @param[in] r Radius in pixels; use `LV_RADIUS_CIRCLE` for a full circle.
	 * @return Reference to the derived object for method chaining.
	 */
	Derived &radius(int32_t r) {
		lv_obj_set_style_radius(self().get(), r, LV_PART_MAIN);
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
		lv_obj_t *target = lv_event_get_target(e);
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
