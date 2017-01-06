#pragma once

#include "boost_defs.hpp"

#include "gcd_utility.hpp"
#include "logger.hpp"
#include "manipulator.hpp"
#include "modifier_flag_manager.hpp"
#include "pointing_button_manager.hpp"
#include "system_preferences.hpp"
#include "types.hpp"
#include "virtual_hid_device_client.hpp"
#include <IOKit/hidsystem/ev_keymap.h>
#include <boost/optional.hpp>
#include <list>
#include <thread>
#include <unordered_map>

namespace manipulator {
class event_manipulator final {
public:
  event_manipulator(const event_manipulator&) = delete;

  event_manipulator(virtual_hid_device_client& virtual_hid_device_client) : virtual_hid_device_client_(virtual_hid_device_client) {
  }

  ~event_manipulator(void) {
  }

  enum class ready_state {
    ready,
    virtual_hid_device_client_is_not_ready,
    virtual_hid_keyboard_is_not_ready,
  };

  ready_state is_ready(void) {
    if (!virtual_hid_device_client_.is_connected()) {
      return ready_state::virtual_hid_device_client_is_not_ready;
    }
    if (!virtual_hid_device_client_.is_virtual_hid_keyboard_initialized()) {
      return ready_state::virtual_hid_keyboard_is_not_ready;
    }
    return ready_state::ready;
  }

  void reset(void) {
    manipulated_keys_.clear();
    manipulated_fn_keys_.clear();

    modifier_flag_manager_.reset();
    modifier_flag_manager_.unlock();

    pointing_button_manager_.reset();

    // Do not call terminate_virtual_hid_keyboard
    virtual_hid_device_client_.terminate_virtual_hid_pointing();
  }

  void reset_modifier_flag_state(void) {
    modifier_flag_manager_.reset();
    // Do not call modifier_flag_manager_.unlock() here.
  }

  void reset_pointing_button_state(void) {
    auto bits = pointing_button_manager_.get_hid_report_bits();
    pointing_button_manager_.reset();
    if (bits) {
      virtual_hid_device_client_.reset_virtual_hid_pointing();
    }
  }

  void set_system_preferences_values(const system_preferences::values& values) {
    std::lock_guard<std::mutex> guard(system_preferences_values_mutex_);

    system_preferences_values_ = values;
  }

  void clear_simple_modifications(void) {
    simple_modifications_.clear();
  }

  void add_simple_modification(krbn::key_code from_key_code, krbn::key_code to_key_code) {
    simple_modifications_.add(from_key_code, to_key_code);
  }

  void clear_fn_function_keys(void) {
    fn_function_keys_.clear();
  }

  void add_fn_function_key(krbn::key_code from_key_code, krbn::key_code to_key_code) {
    fn_function_keys_.add(from_key_code, to_key_code);
  }

  void clear_standalone_keys(void) {
    standalone_keys_.clear();
  }

  void add_standalone_key(krbn::key_code from_key_code, krbn::key_code to_key_code) {
    standalone_keys_.add(from_key_code, to_key_code);
  }

  void clear_one_to_many_mappings(void) {
    one_to_many_mappings_.clear();
  }

  void add_one_to_many_mappings(krbn::key_code from_key_code, krbn::key_code to_key_code) {
    one_to_many_mappings_.add(from_key_code, to_key_code);
  }

  void initialize_virtual_hid_keyboard(krbn::keyboard_type keyboard_type) {
    virtual_hid_device_client_.initialize_virtual_hid_keyboard(keyboard_type);
  }

  void initialize_virtual_hid_pointing(void) {
    virtual_hid_device_client_.initialize_virtual_hid_pointing();
  }

  void terminate_virtual_hid_pointing(void) {
    virtual_hid_device_client_.terminate_virtual_hid_pointing();
  }

  void set_caps_lock_state(bool state) {
    modifier_flag_manager_.manipulate(krbn::modifier_flag::caps_lock,
                                      state ? modifier_flag_manager::operation::lock : modifier_flag_manager::operation::unlock);
  }

  void handle_keyboard_event(device_registry_entry_id device_registry_entry_id,
                             krbn::key_code from_key_code,
                             bool pressed) {
    if (process_one_to_many_mappings(from_key_code, pressed)) {
        return;
    }
    krbn::key_code to_key_code = from_key_code;

    // ----------------------------------------
    // modify keys
    if (!pressed) {
      if (auto key_code = manipulated_keys_.find(device_registry_entry_id, from_key_code)) {
        manipulated_keys_.remove(device_registry_entry_id, from_key_code);
        to_key_code = *key_code;
      }
    } else {
      if (auto key_code = simple_modifications_.get(from_key_code)) {
        manipulated_keys_.add(device_registry_entry_id, from_key_code, *key_code);
        to_key_code = *key_code;
      }
    }

    // ----------------------------------------
    // modify fn+arrow, function keys
    if (!pressed) {
      if (auto key_code = manipulated_fn_keys_.find(device_registry_entry_id, to_key_code)) {
        manipulated_fn_keys_.remove(device_registry_entry_id, to_key_code);
        to_key_code = *key_code;
      }
    } else {
      boost::optional<krbn::key_code> key_code;

      if (modifier_flag_manager_.pressed(krbn::modifier_flag::fn)) {
        switch (to_key_code) {
        case krbn::key_code::return_or_enter:
          key_code = krbn::key_code::keypad_enter;
          break;
        case krbn::key_code::delete_or_backspace:
          key_code = krbn::key_code::delete_forward;
          break;
        case krbn::key_code::right_arrow:
          key_code = krbn::key_code::end;
          break;
        case krbn::key_code::left_arrow:
          key_code = krbn::key_code::home;
          break;
        case krbn::key_code::down_arrow:
          key_code = krbn::key_code::page_down;
          break;
        case krbn::key_code::up_arrow:
          key_code = krbn::key_code::page_up;
          break;
        default:
          break;
        }
      }

      // f1-f12
      {
        auto key_code_value = static_cast<uint32_t>(to_key_code);
        if (kHIDUsage_KeyboardF1 <= key_code_value && key_code_value <= kHIDUsage_KeyboardF12) {
          bool keyboard_fn_state = false;
          {
            std::lock_guard<std::mutex> guard(system_preferences_values_mutex_);
            keyboard_fn_state = system_preferences_values_.get_keyboard_fn_state();
          }

          bool fn_pressed = modifier_flag_manager_.pressed(krbn::modifier_flag::fn);

          if ((fn_pressed && keyboard_fn_state) ||
              (!fn_pressed && !keyboard_fn_state)) {
            // change f1-f12 keys to media controls
            if (auto k = fn_function_keys_.get(to_key_code)) {
              key_code = *k;
            }
          }
        }
      }

      if (key_code) {
        manipulated_fn_keys_.add(device_registry_entry_id, to_key_code, *key_code);
        to_key_code = *key_code;
      }
    }

    // ----------------------------------------
    if (process_standalone_key(from_key_code, to_key_code, pressed)) {
      return;
    }

    post_key(to_key_code, pressed);
  }

  void handle_pointing_event(device_registry_entry_id device_registry_entry_id,
                             krbn::pointing_event pointing_event,
                             boost::optional<krbn::pointing_button> pointing_button,
                             CFIndex integer_value) {
    pqrs::karabiner_virtual_hid_device::hid_report::pointing_input report;

    switch (pointing_event) {
    case krbn::pointing_event::button:
      if (pointing_button && *pointing_button != krbn::pointing_button::zero) {
        pointing_button_manager_.manipulate(*pointing_button,
                                            integer_value ? pointing_button_manager::operation::increase : pointing_button_manager::operation::decrease);
      }
      break;

    case krbn::pointing_event::x:
      report.x = integer_value;
      break;

    case krbn::pointing_event::y:
      report.y = integer_value;
      break;

    case krbn::pointing_event::vertical_wheel:
      report.vertical_wheel = integer_value;
      break;

    case krbn::pointing_event::horizontal_wheel:
      report.horizontal_wheel = integer_value;
      break;

    default:
      break;
    }

    auto bits = pointing_button_manager_.get_hid_report_bits();
    report.buttons[0] = (bits >> 0) & 0xff;
    report.buttons[1] = (bits >> 8) & 0xff;
    report.buttons[2] = (bits >> 16) & 0xff;
    report.buttons[3] = (bits >> 24) & 0xff;
    virtual_hid_device_client_.post_pointing_input_report(report);
  }

  void stop_key_repeat(void) {
    virtual_hid_device_client_.reset_virtual_hid_keyboard();
  }

private:
  class manipulated_keys final {
  public:
    manipulated_keys(const manipulated_keys&) = delete;

    manipulated_keys(void) {
    }

    void clear(void) {
      std::lock_guard<std::mutex> guard(mutex_);

      manipulated_keys_.clear();
    }

    void add(device_registry_entry_id device_registry_entry_id,
             krbn::key_code from_key_code,
             krbn::key_code to_key_code) {
      std::lock_guard<std::mutex> guard(mutex_);

      manipulated_keys_.push_back(manipulated_key(device_registry_entry_id, from_key_code, to_key_code));
    }

    boost::optional<krbn::key_code> find(device_registry_entry_id device_registry_entry_id,
                                         krbn::key_code from_key_code) {
      std::lock_guard<std::mutex> guard(mutex_);

      for (const auto& v : manipulated_keys_) {
        if (v.get_device_registry_entry_id() == device_registry_entry_id &&
            v.get_from_key_code() == from_key_code) {
          return v.get_to_key_code();
        }
      }
      return boost::none;
    }

    void remove(device_registry_entry_id device_registry_entry_id,
                krbn::key_code from_key_code) {
      std::lock_guard<std::mutex> guard(mutex_);

      manipulated_keys_.remove_if([&](const manipulated_key& v) {
        return v.get_device_registry_entry_id() == device_registry_entry_id &&
               v.get_from_key_code() == from_key_code;
      });
    }

  private:
    class manipulated_key final {
    public:
      manipulated_key(device_registry_entry_id device_registry_entry_id,
                      krbn::key_code from_key_code,
                      krbn::key_code to_key_code) : device_registry_entry_id_(device_registry_entry_id),
                                                    from_key_code_(from_key_code),
                                                    to_key_code_(to_key_code) {
      }

      device_registry_entry_id get_device_registry_entry_id(void) const { return device_registry_entry_id_; }
      krbn::key_code get_from_key_code(void) const { return from_key_code_; }
      krbn::key_code get_to_key_code(void) const { return to_key_code_; }

    private:
      device_registry_entry_id device_registry_entry_id_;
      krbn::key_code from_key_code_;
      krbn::key_code to_key_code_;
    };

    std::list<manipulated_key> manipulated_keys_;
    std::mutex mutex_;
  };

  class simple_modifications final {
  public:
    simple_modifications(const simple_modifications&) = delete;

    simple_modifications(void) {
    }

    void clear(void) {
      std::lock_guard<std::mutex> guard(mutex_);

      map_.clear();
    }

    void add(krbn::key_code from_key_code, krbn::key_code to_key_code) {
      std::lock_guard<std::mutex> guard(mutex_);

      map_[from_key_code] = to_key_code;
    }

    boost::optional<krbn::key_code> get(krbn::key_code from_key_code) {
      std::lock_guard<std::mutex> guard(mutex_);

      auto it = map_.find(from_key_code);
      if (it != map_.end()) {
        return it->second;
      }

      return boost::none;
    }

  private:
    std::unordered_map<krbn::key_code, krbn::key_code> map_;
    std::mutex mutex_;
  };

  class one_to_many_mappings final {
  public:
    one_to_many_mappings(const one_to_many_mappings&) = delete;

    one_to_many_mappings(void) {
    }

    void clear(void) {
      std::lock_guard<std::mutex> guard(mutex_);

      map_.clear();
    }

    void add(krbn::key_code from_key_code, krbn::key_code to_key_code) {
      std::lock_guard<std::mutex> guard(mutex_);

      auto it = map_.find(from_key_code);
      if (it != map_.end()) {
          it->second.push_back(to_key_code);
      } else {
          std::vector<krbn::key_code> to_key_codes;
          to_key_codes.push_back(to_key_code);
          map_[from_key_code] = to_key_codes;
      }
    }

    boost::optional<std::vector<krbn::key_code>> get(krbn::key_code from_key_code) {
      std::lock_guard<std::mutex> guard(mutex_);

      auto it = map_.find(from_key_code);
      if (it != map_.end()) {
        return it->second;
      }

      return boost::none;
    }

  private:
    std::unordered_map<krbn::key_code, std::vector<krbn::key_code>> map_;
    std::mutex mutex_;
  };

  bool process_one_to_many_mappings(krbn::key_code key_code, bool pressed) {
    if (auto to_key_codes = one_to_many_mappings_.get(key_code)) {
      for (auto it = (*to_key_codes).begin(); it != (*to_key_codes).end(); it++) {
        post_key(*it, pressed);
      }
      return true;
    }
    return false;
  }

  bool process_standalone_key(krbn::key_code from_key_code, krbn::key_code to_key_code, bool pressed) {
    if (pressed) {
      if (!standalone_keys_.get(from_key_code) || (standalone_keys_timer_ != nullptr && from_key_code != standalone_from_key_)) {
        if (standalone_keys_timer_ != nullptr) {
          standalone_keys_timer_ = nullptr;
          post_key(standalone_to_key_, pressed);
        }
        return false;
      } else {
        standalone_from_key_ = from_key_code;
        standalone_to_key_ = to_key_code;
        long standalone_key_milliseconds = system_preferences_values_.get_standalone_key_milliseconds();
        standalone_keys_timer_ = std::make_unique<gcd_utility::main_queue_timer>(
            DISPATCH_TIMER_STRICT,
            dispatch_time(DISPATCH_TIME_NOW, standalone_key_milliseconds * NSEC_PER_MSEC),
            standalone_key_milliseconds * NSEC_PER_MSEC,
            0,
            ^{
              post_key(to_key_code, pressed);
              standalone_keys_timer_ = nullptr;
            });
        return true;
      }
    } else {
      if (from_key_code == standalone_from_key_ && standalone_keys_timer_ != nullptr) {
        standalone_keys_timer_ = nullptr;
        auto to_standalone_key_code = standalone_keys_.get(from_key_code);
        post_key(*to_standalone_key_code, true);
        post_key(*to_standalone_key_code, false);
        return true;
      } else {
        return false;
      }
    }
  }

  bool post_standalone_key(krbn::key_code key_code) {
    if (auto to_key_code = standalone_keys_.get(key_code)) {
      post_key(*to_key_code, true);
      post_key(*to_key_code, false);
      return true;
    }
    return false;
  }

  void post_key(krbn::key_code key_code, bool pressed) {
    // pre-process modifier keys
    auto operation = pressed ? manipulator::modifier_flag_manager::operation::increase : manipulator::modifier_flag_manager::operation::decrease;
    auto modifier_flag = krbn::types::get_modifier_flag(key_code);
    if (modifier_flag != krbn::modifier_flag::zero) {
      modifier_flag_manager_.manipulate(modifier_flag, operation);
    }

    // send key event
    if (auto usage_page = krbn::types::get_usage_page(key_code)) {
      if (auto usage = krbn::types::get_usage(key_code)) {
        pqrs::karabiner_virtual_hid_device::hid_event_service::keyboard_event keyboard_event;
        keyboard_event.usage_page = *usage_page;
        keyboard_event.usage = *usage;
        keyboard_event.value = pressed;
        virtual_hid_device_client_.dispatch_keyboard_event(keyboard_event);
      }
    }
  }

  virtual_hid_device_client& virtual_hid_device_client_;
  modifier_flag_manager modifier_flag_manager_;
  pointing_button_manager pointing_button_manager_;

  system_preferences::values system_preferences_values_;
  std::mutex system_preferences_values_mutex_;

  simple_modifications simple_modifications_;
  simple_modifications fn_function_keys_;
  simple_modifications standalone_keys_;
  one_to_many_mappings one_to_many_mappings_;
  krbn::key_code standalone_from_key_;
  krbn::key_code standalone_to_key_;
  std::unique_ptr<gcd_utility::main_queue_timer> standalone_keys_timer_;

  manipulated_keys manipulated_keys_;
  manipulated_keys manipulated_fn_keys_;
};
}
