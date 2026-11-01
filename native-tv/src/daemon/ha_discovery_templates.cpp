#include "daemon/ha_discovery.hpp"

namespace nativetv { namespace daemon {
namespace {
const Json &templates() {
    // Generated from scripts/discovery-configs.js using the C2 settings fixture.
    static const Json value = Json::parse(R"GHJSON([
  {
    "type": "sensor",
    "id": "soc_temperature",
    "conf": {
      "name": "SoC Temperature",
      "state_topic": "tv/telemetry",
      "value_template": "{{ value_json.get('temp') }}",
      "unit_of_measurement": "°C",
      "device_class": "temperature",
      "state_class": "measurement",
      "unique_id": "lg_tv_soc_temperature",
      "default_entity_id": "sensor.lg_tv_soc_temperature",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'online' if value in ['online', 'off'] else 'offline' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "sensor",
    "id": "cpu_load",
    "conf": {
      "name": "CPU Usage",
      "state_topic": "tv/telemetry",
      "value_template": "{{ value_json.get('load') }}",
      "unit_of_measurement": "%",
      "state_class": "measurement",
      "icon": "mdi:cpu-64-bit",
      "unique_id": "lg_tv_cpu_load",
      "default_entity_id": "sensor.lg_tv_cpu_load",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'online' if value in ['online', 'off'] else 'offline' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "sensor",
    "id": "memory_usage",
    "conf": {
      "name": "Memory Usage",
      "state_topic": "tv/telemetry",
      "value_template": "{{ (((value_json.get('mem') or {}).get('total') - (value_json.get('mem') or {}).get('avail')) / (value_json.get('mem') or {}).get('total') * 100) | round(1) if (value_json.get('mem') or {}).get('total') > 0 else 0 }}",
      "unit_of_measurement": "%",
      "icon": "mdi:memory",
      "unique_id": "lg_tv_memory_usage",
      "default_entity_id": "sensor.lg_tv_memory_usage",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'online' if value in ['online', 'off'] else 'offline' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "sensor",
    "id": "swap_usage",
    "conf": {
      "name": "Swap Usage",
      "state_topic": "tv/telemetry",
      "value_template": "{{ (((value_json.get('swap') or {}).get('total') - (value_json.get('swap') or {}).get('free')) / (value_json.get('swap') or {}).get('total') * 100) | round(1) if (value_json.get('swap') or {}).get('total') > 0 else 0 }}",
      "unit_of_measurement": "%",
      "icon": "mdi:server",
      "unique_id": "lg_tv_swap_usage",
      "default_entity_id": "sensor.lg_tv_swap_usage",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'online' if value in ['online', 'off'] else 'offline' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "sensor",
    "id": "wifi_signal",
    "conf": {
      "name": "Wi-Fi Signal",
      "state_topic": "tv/telemetry",
      "value_template": "{{ (value_json.get('wifi') or {}).get('level') if value_json.get('wifi') else none }}",
      "unit_of_measurement": "dBm",
      "device_class": "signal_strength",
      "state_class": "measurement",
      "unique_id": "lg_tv_wifi_signal",
      "default_entity_id": "sensor.lg_tv_wifi_signal",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'online' if value in ['online', 'off'] else 'offline' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "sensor",
    "id": "download_rate",
    "conf": {
      "name": "Download Rate",
      "state_topic": "tv/telemetry",
      "value_template": "{{ ((value_json.get('net') or {}).get('rx') / 1024) | round(1) if value_json.get('net') else 0 }}",
      "unit_of_measurement": "kB/s",
      "icon": "mdi:download-network",
      "unique_id": "lg_tv_download_rate",
      "default_entity_id": "sensor.lg_tv_download_rate",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'online' if value in ['online', 'off'] else 'offline' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "sensor",
    "id": "upload_rate",
    "conf": {
      "name": "Upload Rate",
      "state_topic": "tv/telemetry",
      "value_template": "{{ ((value_json.get('net') or {}).get('tx') / 1024) | round(1) if value_json.get('net') else 0 }}",
      "unit_of_measurement": "kB/s",
      "icon": "mdi:upload-network",
      "unique_id": "lg_tv_upload_rate",
      "default_entity_id": "sensor.lg_tv_upload_rate",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'online' if value in ['online', 'off'] else 'offline' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "sensor",
    "id": "flash_health",
    "conf": {
      "name": "Flash Storage Health",
      "state_topic": "tv/telemetry",
      "value_template": "{{ (value_json.get('emmc') or {}).get('eol') }}",
      "icon": "mdi:harddisk",
      "unique_id": "lg_tv_flash_health",
      "default_entity_id": "sensor.lg_tv_flash_health",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'offline' if value == 'offline' else 'online' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "sensor",
    "id": "flash_wear",
    "conf": {
      "name": "Flash Wear Level",
      "state_topic": "tv/telemetry",
      "value_template": "{{ (value_json.get('emmc') or {}).get('wear') }}",
      "icon": "mdi:wrench-clock",
      "unique_id": "lg_tv_flash_wear",
      "default_entity_id": "sensor.lg_tv_flash_wear",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'offline' if value == 'offline' else 'online' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "sensor",
    "id": "active_app",
    "conf": {
      "name": "Active App",
      "state_topic": "tv/telemetry",
      "value_template": "{{ \"Off\" if value_json.get('tvOff') else (value_json.get('display_title') or value_json.get('app_name') or value_json.get('app')) }}",
      "json_attributes_topic": "tv/state/application/remote",
      "json_attributes_template": "{{ value_json | tojson }}",
      "icon": "mdi:television-play",
      "unique_id": "lg_tv_active_app",
      "default_entity_id": "sensor.lg_tv_active_app",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'offline' if value == 'offline' else 'online' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "sensor",
    "id": "play_state",
    "conf": {
      "name": "Player State",
      "state_topic": "tv/telemetry",
      "value_template": "{{ \"Off\" if value_json.get('tvOff') else ((value_json.get('media') or {}).get('state') if value_json.get('media') else None) }}",
      "icon": "mdi:play-pause",
      "unique_id": "lg_tv_play_state",
      "default_entity_id": "sensor.lg_tv_play_state",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'offline' if value == 'offline' else 'online' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "sensor",
    "id": "dynamic_range",
    "conf": {
      "name": "Dynamic Range",
      "state_topic": "tv/telemetry",
      "value_template": "{{ \"Off\" if value_json.get('tvOff') else ((value_json.get('picture') or {}).get('dynamicRange') if value_json.get('picture') else \"SDR\") }}",
      "icon": "mdi:video-vintage",
      "unique_id": "lg_tv_dynamic_range",
      "default_entity_id": "sensor.lg_tv_dynamic_range",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'offline' if value == 'offline' else 'online' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "sensor",
    "id": "picture_mode",
    "conf": {
      "name": "Picture Mode",
      "state_topic": "tv/telemetry",
      "value_template": "{{ (value_json.get('picture') or {}).get('mode') if value_json.get('picture') else \"Unknown\" }}",
      "icon": "mdi:palette",
      "unique_id": "lg_tv_picture_mode",
      "default_entity_id": "sensor.lg_tv_picture_mode",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'offline' if value == 'offline' else 'online' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "number",
    "id": "oled_light",
    "conf": {
      "name": "OLED Light",
      "command_topic": "tv/command/backlight",
      "state_topic": "tv/state/picture/backlight",
      "min": 0,
      "max": 100,
      "step": 1,
      "mode": "slider",
      "unit_of_measurement": "%",
      "icon": "mdi:brightness-6",
      "unique_id": "lg_tv_oled_light",
      "default_entity_id": "number.lg_tv_oled_light",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'online' if value in ['online', 'off'] else 'offline' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "sensor",
    "id": "video_signal",
    "conf": {
      "name": "Video Signal",
      "state_topic": "tv/telemetry",
      "value_template": "{{ \"Off\" if value_json.get('tvOff') else (value_json.get('signal') or \"Internal / Standby\") }}",
      "icon": "mdi:video-input-hdmi",
      "unique_id": "lg_tv_video_signal",
      "default_entity_id": "sensor.lg_tv_video_signal",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'offline' if value == 'offline' else 'online' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "sensor",
    "id": "hdmi_link_mode",
    "conf": {
      "name": "HDMI Link Protocol",
      "state_topic": "tv/telemetry",
      "value_template": "{{ \"Off\" if value_json.get('tvOff') else ((value_json.get('hdmi_diag') or {}).get('phy_mode') if value_json.get('hdmi_diag') and (value_json.get('hdmi_diag') or {}).get('phy_mode') else none) }}",
      "entity_category": "diagnostic",
      "icon": "mdi:video-input-hdmi",
      "unique_id": "lg_tv_hdmi_link_mode",
      "default_entity_id": "sensor.lg_tv_hdmi_link_mode",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'offline' if value == 'offline' else 'online' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "sensor",
    "id": "hdmi_chroma",
    "conf": {
      "name": "HDMI Chroma Format",
      "state_topic": "tv/telemetry",
      "value_template": "{{ \"Off\" if value_json.get('tvOff') else ((value_json.get('hdmi_diag') or {}).get('chroma') if value_json.get('hdmi_diag') and (value_json.get('hdmi_diag') or {}).get('chroma') else none) }}",
      "entity_category": "diagnostic",
      "icon": "mdi:palette",
      "unique_id": "lg_tv_hdmi_chroma",
      "default_entity_id": "sensor.lg_tv_hdmi_chroma",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'offline' if value == 'offline' else 'online' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "sensor",
    "id": "hdmi_hdcp",
    "conf": {
      "name": "HDMI HDCP Version",
      "state_topic": "tv/telemetry",
      "value_template": "{{ \"Off\" if value_json.get('tvOff') else ((value_json.get('hdmi_diag') or {}).get('hdcp') if value_json.get('hdmi_diag') and (value_json.get('hdmi_diag') or {}).get('hdcp') else none) }}",
      "entity_category": "diagnostic",
      "icon": "mdi:lock-check",
      "unique_id": "lg_tv_hdmi_hdcp",
      "default_entity_id": "sensor.lg_tv_hdmi_hdcp",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'offline' if value == 'offline' else 'online' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "binary_sensor",
    "id": "hdmi_allm",
    "conf": {
      "name": "Auto Low Latency Mode (ALLM)",
      "state_topic": "tv/telemetry",
      "value_template": "{{ \"OFF\" if value_json.get('tvOff') else ((\"ON\" if (value_json.get('hdmi_diag') or {}).get('allm') else \"OFF\") if value_json.get('hdmi_diag') and (value_json.get('hdmi_diag') or {}).get('allm') is not none else none) }}",
      "icon": "mdi:gamepad-variant",
      "unique_id": "lg_tv_hdmi_allm",
      "default_entity_id": "binary_sensor.lg_tv_hdmi_allm",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'offline' if value == 'offline' else 'online' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "binary_sensor",
    "id": "hdmi_vrr",
    "conf": {
      "name": "Variable Refresh Rate (VRR)",
      "state_topic": "tv/telemetry",
      "value_template": "{{ \"OFF\" if value_json.get('tvOff') else ((\"ON\" if (value_json.get('hdmi_diag') or {}).get('vrr') else \"OFF\") if value_json.get('hdmi_diag') and (value_json.get('hdmi_diag') or {}).get('vrr') is not none else none) }}",
      "icon": "mdi:speedometer",
      "unique_id": "lg_tv_hdmi_vrr",
      "default_entity_id": "binary_sensor.lg_tv_hdmi_vrr",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'offline' if value == 'offline' else 'online' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "sensor",
    "id": "video_colorimetry",
    "conf": {
      "name": "Video Color Space",
      "state_topic": "tv/telemetry",
      "value_template": "{{ \"Off\" if value_json.get('tvOff') else ((value_json.get('picture_engine') or {}).get('colorimetry') if value_json.get('picture_engine') and (value_json.get('picture_engine') or {}).get('colorimetry') else none) }}",
      "icon": "mdi:palette-swatch",
      "unique_id": "lg_tv_video_colorimetry",
      "default_entity_id": "sensor.lg_tv_video_colorimetry",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'offline' if value == 'offline' else 'online' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "sensor",
    "id": "sound_format",
    "conf": {
      "name": "Sound Format",
      "state_topic": "tv/telemetry",
      "value_template": "{% set i = ((value_json.get('sound') or {}).get('input') if value_json.get('sound') else none) or {} %}{{ ((i.get(\"codec\") or \"\") ~ (\" · \" ~ i.get(\"immersive\") if i.get(\"immersive\") else \"\")) if i else \"None\" }}",
      "json_attributes_topic": "tv/telemetry",
      "json_attributes_template": "{% set i = (value_json.get('sound') or {}).get('input') or {} %}{{ {\"codec\": i.get(\"codec_raw\") or none, \"immersive\": i.get(\"immersive_raw\") or none} | tojson }}",
      "icon": "mdi:surround-sound",
      "unique_id": "lg_tv_sound_format",
      "default_entity_id": "sensor.lg_tv_sound_format",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'offline' if value == 'offline' else 'online' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "sensor",
    "id": "audio_output",
    "conf": {
      "name": "Audio Output",
      "state_topic": "tv/telemetry",
      "value_template": "{{ value_json.get('audio_output') or \"Internal\" }}",
      "icon": "mdi:speaker",
      "unique_id": "lg_tv_audio_output",
      "default_entity_id": "sensor.lg_tv_audio_output",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'offline' if value == 'offline' else 'online' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "sensor",
    "id": "uptime",
    "conf": {
      "name": "Uptime",
      "state_topic": "tv/telemetry",
      "value_template": "{{ value_json.get('bootTime') }}",
      "device_class": "timestamp",
      "entity_category": "diagnostic",
      "icon": "mdi:clock-start",
      "unique_id": "lg_tv_uptime",
      "default_entity_id": "sensor.lg_tv_uptime",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'offline' if value == 'offline' else 'online' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "sensor",
    "id": "watch_time_today",
    "conf": {
      "name": "Watch Time Today",
      "state_topic": "tv/telemetry",
      "value_template": "{{ (value_json.get('watch') or {}).get('today_h') if value_json.get('watch') else none }}",
      "unit_of_measurement": "h",
      "device_class": "duration",
      "state_class": "total_increasing",
      "icon": "mdi:television-classic",
      "unique_id": "lg_tv_watch_time_today",
      "default_entity_id": "sensor.lg_tv_watch_time_today",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'offline' if value == 'offline' else 'online' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "sensor",
    "id": "watch_time_week",
    "conf": {
      "name": "Watch Time, Last 7 Days",
      "state_topic": "tv/telemetry",
      "value_template": "{{ (value_json.get('watch') or {}).get('week_h') if value_json.get('watch') else none }}",
      "unit_of_measurement": "h",
      "device_class": "duration",
      "state_class": "measurement",
      "icon": "mdi:calendar-week",
      "unique_id": "lg_tv_watch_time_week",
      "default_entity_id": "sensor.lg_tv_watch_time_week",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'offline' if value == 'offline' else 'online' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "sensor",
    "id": "watch_top_app_today",
    "conf": {
      "name": "Most Watched Today",
      "state_topic": "tv/telemetry",
      "value_template": "{{ (value_json.get('watch') or {}).get('top_today') if value_json.get('watch') and (value_json.get('watch') or {}).get('top_today') else none }}",
      "icon": "mdi:trophy-outline",
      "unique_id": "lg_tv_watch_top_app_today",
      "default_entity_id": "sensor.lg_tv_watch_top_app_today",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'offline' if value == 'offline' else 'online' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "sensor",
    "id": "tvweb_version",
    "conf": {
      "name": "Server Version",
      "state_topic": "tv/telemetry",
      "value_template": "{{ value_json.get('tvwebVersion') }}",
      "entity_category": "diagnostic",
      "icon": "mdi:tag-outline",
      "unique_id": "lg_tv_tvweb_version",
      "default_entity_id": "sensor.lg_tv_tvweb_version",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'offline' if value == 'offline' else 'online' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "sensor",
    "id": "mac_address",
    "conf": {
      "name": "MAC Address",
      "state_topic": "tv/telemetry",
      "value_template": "{{ value_json.get('mac') if value_json.get('mac') else none }}",
      "entity_category": "diagnostic",
      "icon": "mdi:ethernet",
      "unique_id": "lg_tv_mac_address",
      "default_entity_id": "sensor.lg_tv_mac_address",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'offline' if value == 'offline' else 'online' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "sensor",
    "id": "remote_battery",
    "conf": {
      "name": "Remote Battery",
      "state_topic": "tv/telemetry",
      "value_template": "{{ (value_json.get('remote') or {}).get('battery') if value_json.get('remote') and (value_json.get('remote') or {}).get('battery') is not none else none }}",
      "unit_of_measurement": "%",
      "device_class": "battery",
      "state_class": "measurement",
      "entity_category": "diagnostic",
      "icon": "mdi:remote",
      "unique_id": "lg_tv_remote_battery",
      "default_entity_id": "sensor.lg_tv_remote_battery",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'offline' if value == 'offline' else 'online' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "sensor",
    "id": "soc_architecture",
    "conf": {
      "name": "SoC Architecture",
      "state_topic": "tv/telemetry",
      "value_template": "{{ (value_json.get('hardware') or {}).get('soc_arch') if value_json.get('hardware') and (value_json.get('hardware') or {}).get('soc_arch') else \"Unknown\" }}",
      "entity_category": "diagnostic",
      "icon": "mdi:cpu-64-bit",
      "unique_id": "lg_tv_soc_architecture",
      "default_entity_id": "sensor.lg_tv_soc_architecture",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'offline' if value == 'offline' else 'online' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "sensor",
    "id": "oled_cell_type",
    "conf": {
      "name": "OLED Cell Info",
      "state_topic": "tv/telemetry",
      "value_template": "{{ (value_json.get('panel_silicon') or {}).get('cell') if value_json.get('panel_silicon') and (value_json.get('panel_silicon') or {}).get('cell') else none }}",
      "entity_category": "diagnostic",
      "icon": "mdi:monitor-cell",
      "unique_id": "lg_tv_oled_cell_type",
      "default_entity_id": "sensor.lg_tv_oled_cell_type",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'offline' if value == 'offline' else 'online' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "sensor",
    "id": "tcon_firmware",
    "conf": {
      "name": "TCON Firmware",
      "state_topic": "tv/telemetry",
      "value_template": "{{ (value_json.get('panel_silicon') or {}).get('tcon_firmware') if value_json.get('panel_silicon') and (value_json.get('panel_silicon') or {}).get('tcon_firmware') else none }}",
      "entity_category": "diagnostic",
      "icon": "mdi:chip",
      "unique_id": "lg_tv_tcon_firmware",
      "default_entity_id": "sensor.lg_tv_tcon_firmware",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'offline' if value == 'offline' else 'online' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "switch",
    "id": "display_panel",
    "conf": {
      "name": "OLED Display Panel",
      "command_topic": "tv/command/screen",
      "state_topic": "tv/state/screen",
      "payload_on": "ON",
      "payload_off": "OFF",
      "icon": "mdi:television-ambient-light",
      "unique_id": "lg_tv_display_panel",
      "default_entity_id": "switch.lg_tv_display_panel",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'online' if value in ['online', 'off'] else 'offline' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "switch",
    "id": "mute",
    "conf": {
      "name": "Mute",
      "command_topic": "tv/command/mute",
      "state_topic": "tv/telemetry",
      "value_template": "{{ 'ON' if value_json.get('muted') else 'OFF' }}",
      "payload_on": "ON",
      "payload_off": "OFF",
      "icon": "mdi:volume-mute",
      "availability": [
        {
          "topic": "tv/status",
          "value_template": "{{ 'online' if value in ['online', 'off'] else 'offline' }}"
        },
        {
          "topic": "tv/telemetry",
          "value_template": "{{ \"offline\" if value_json.get('volume_control') == \"none\" else \"online\" }}"
        }
      ],
      "unique_id": "lg_tv_mute",
      "default_entity_id": "switch.lg_tv_mute",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_mode": "all"
    }
  },
  {
    "type": "number",
    "id": "volume",
    "conf": {
      "name": "Volume",
      "command_topic": "tv/command/volume",
      "state_topic": "tv/telemetry",
      "value_template": "{{ value_json.get('volume') }}",
      "min": 0,
      "max": 100,
      "step": 1,
      "icon": "mdi:volume-high",
      "availability": [
        {
          "topic": "tv/status",
          "value_template": "{{ 'online' if value in ['online', 'off'] else 'offline' }}"
        },
        {
          "topic": "tv/telemetry",
          "value_template": "{{ \"online\" if (value_json.get('volume_control') or \"level\") == \"level\" else \"offline\" }}"
        }
      ],
      "unique_id": "lg_tv_volume",
      "default_entity_id": "number.lg_tv_volume",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_mode": "all"
    }
  },
  {
    "type": "select",
    "id": "input_source",
    "conf": {
      "name": "Input Source",
      "command_topic": "tv/command/input",
      "state_topic": "tv/telemetry",
      "icon": "mdi:video-input-hdmi",
      "options": [
        "HDMI 1",
        "HDMI 2",
        "HDMI 3",
        "HDMI 4",
        "Live TV"
      ],
      "command_template": "{{ {\"HDMI 1\":\"hdmi1\",\"HDMI 2\":\"hdmi2\",\"HDMI 3\":\"hdmi3\",\"HDMI 4\":\"hdmi4\",\"Live TV\":\"livetv\"}.get(value, value) }}",
      "value_template": "{{ {\"hdmi1\":\"HDMI 1\",\"hdmi2\":\"HDMI 2\",\"hdmi3\":\"HDMI 3\",\"hdmi4\":\"HDMI 4\",\"livetv\":\"Live TV\"}.get(value_json.get('app'), \"None\") }}",
      "unique_id": "lg_tv_input_source",
      "default_entity_id": "select.lg_tv_input_source",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'online' if value in ['online', 'off'] else 'offline' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "text",
    "id": "screen_notification",
    "conf": {
      "name": "Screen Notification",
      "command_topic": "tv/command/toast",
      "icon": "mdi:message-text-outline",
      "mode": "text",
      "unique_id": "lg_tv_screen_notification",
      "default_entity_id": "text.lg_tv_screen_notification",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'online' if value in ['online', 'off'] else 'offline' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "sensor",
    "id": "oled_panel_hours",
    "conf": {
      "name": "OLED Panel Hours",
      "state_topic": "tv/telemetry",
      "value_template": "{{ (value_json.get('oled') or {}).get('panel_hours') if value_json.get('oled') else 0 }}",
      "unit_of_measurement": "h",
      "state_class": "total_increasing",
      "icon": "mdi:timer-outline",
      "unique_id": "lg_tv_oled_panel_hours",
      "default_entity_id": "sensor.lg_tv_oled_panel_hours",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'offline' if value == 'offline' else 'online' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "sensor",
    "id": "oled_hours_since_compensation",
    "conf": {
      "name": "OLED Hours Since Short Cycle",
      "state_topic": "tv/telemetry",
      "value_template": "{{ (value_json.get('oled') or {}).get('hours_since_comp') if value_json.get('oled') else 0 }}",
      "unit_of_measurement": "h",
      "state_class": "measurement",
      "icon": "mdi:progress-clock",
      "unique_id": "lg_tv_oled_hours_since_compensation",
      "default_entity_id": "sensor.lg_tv_oled_hours_since_compensation",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'offline' if value == 'offline' else 'online' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "sensor",
    "id": "oled_hours_until_compensation",
    "conf": {
      "name": "OLED Hours Until Short Cycle",
      "state_topic": "tv/telemetry",
      "value_template": "{{ (value_json.get('oled') or {}).get('hours_until_comp') if value_json.get('oled') else 0 }}",
      "unit_of_measurement": "h",
      "state_class": "measurement",
      "icon": "mdi:timer-sand",
      "unique_id": "lg_tv_oled_hours_until_compensation",
      "default_entity_id": "sensor.lg_tv_oled_hours_until_compensation",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'offline' if value == 'offline' else 'online' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "sensor",
    "id": "oled_hours_since_refresher",
    "conf": {
      "name": "OLED Hours Since Pixel Refresher",
      "state_topic": "tv/telemetry",
      "value_template": "{{ (value_json.get('oled') or {}).get('hours_since_refresher') if value_json.get('oled') else 0 }}",
      "unit_of_measurement": "h",
      "state_class": "measurement",
      "icon": "mdi:history",
      "unique_id": "lg_tv_oled_hours_since_refresher",
      "default_entity_id": "sensor.lg_tv_oled_hours_since_refresher",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'offline' if value == 'offline' else 'online' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "sensor",
    "id": "oled_hours_until_refresher",
    "conf": {
      "name": "OLED Hours Until Pixel Refresher",
      "state_topic": "tv/telemetry",
      "value_template": "{{ (value_json.get('oled') or {}).get('hours_until_refresher') if value_json.get('oled') else 0 }}",
      "unit_of_measurement": "h",
      "state_class": "measurement",
      "icon": "mdi:update",
      "unique_id": "lg_tv_oled_hours_until_refresher",
      "default_entity_id": "sensor.lg_tv_oled_hours_until_refresher",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'offline' if value == 'offline' else 'online' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "sensor",
    "id": "oled_compensation_status",
    "conf": {
      "name": "OLED Compensation Status",
      "state_topic": "tv/telemetry",
      "value_template": "{{ (value_json.get('oled') or {}).get('comp_status') if value_json.get('oled') else \"Unknown\" }}",
      "icon": "mdi:autorenew",
      "unique_id": "lg_tv_oled_compensation_status",
      "default_entity_id": "sensor.lg_tv_oled_compensation_status",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'offline' if value == 'offline' else 'online' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "sensor",
    "id": "oled_refresher_status",
    "conf": {
      "name": "Pixel Refresher Status",
      "state_topic": "tv/telemetry",
      "value_template": "{{ (value_json.get('oled') or {}).get('refresher_status') if value_json.get('oled') else \"Unknown\" }}",
      "icon": "mdi:television-shimmer",
      "unique_id": "lg_tv_oled_refresher_status",
      "default_entity_id": "sensor.lg_tv_oled_refresher_status",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'offline' if value == 'offline' else 'online' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "switch",
    "id": "oled_screen_shift",
    "conf": {
      "name": "OLED Screen Shift",
      "command_topic": "tv/command/screenShift",
      "state_topic": "tv/telemetry",
      "value_template": "{{ (\"ON\" if (value_json.get('oled') or {}).get('screen_shift') == \"on\" else \"OFF\") if value_json.get('oled') and (value_json.get('oled') or {}).get('screen_shift') else none }}",
      "payload_on": "on",
      "payload_off": "off",
      "state_on": "ON",
      "state_off": "OFF",
      "icon": "mdi:arrow-all",
      "unique_id": "lg_tv_oled_screen_shift",
      "default_entity_id": "switch.lg_tv_oled_screen_shift",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'online' if value in ['online', 'off'] else 'offline' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "select",
    "id": "oled_logo_dimming",
    "conf": {
      "name": "OLED Logo Dimming",
      "command_topic": "tv/command/logoDimming",
      "state_topic": "tv/telemetry",
      "options": [
        "Off",
        "Light",
        "High"
      ],
      "command_template": "{{ {\"Off\":\"off\",\"Light\":\"light\",\"High\":\"strong\"}[value] }}",
      "value_template": "{{ {\"off\":\"Off\",\"light\":\"Light\",\"strong\":\"High\"}.get((value_json.get('oled') or {}).get('logo_dimming'), \"Off\") if value_json.get('oled') and (value_json.get('oled') or {}).get('logo_dimming') else none }}",
      "icon": "mdi:television-guide",
      "unique_id": "lg_tv_oled_logo_dimming",
      "default_entity_id": "select.lg_tv_oled_logo_dimming",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'online' if value in ['online', 'off'] else 'offline' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "sensor",
    "id": "oled_short_cycles",
    "conf": {
      "name": "OLED Short Cycles Completed",
      "state_topic": "tv/telemetry",
      "value_template": "{{ (value_json.get('oled') or {}).get('comp_cycles') if value_json.get('oled') and (value_json.get('oled') or {}).get('comp_cycles') is not none else none }}",
      "state_class": "total_increasing",
      "entity_category": "diagnostic",
      "icon": "mdi:counter",
      "unique_id": "lg_tv_oled_short_cycles",
      "default_entity_id": "sensor.lg_tv_oled_short_cycles",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'offline' if value == 'offline' else 'online' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "sensor",
    "id": "oled_refresher_cycles",
    "conf": {
      "name": "OLED Refresher Cycles Completed",
      "state_topic": "tv/telemetry",
      "value_template": "{{ (value_json.get('oled') or {}).get('refresher_cycles') if value_json.get('oled') and (value_json.get('oled') or {}).get('refresher_cycles') is not none else none }}",
      "state_class": "total_increasing",
      "entity_category": "diagnostic",
      "icon": "mdi:counter",
      "unique_id": "lg_tv_oled_refresher_cycles",
      "default_entity_id": "sensor.lg_tv_oled_refresher_cycles",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'offline' if value == 'offline' else 'online' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "sensor",
    "id": "oled_failure_alerts",
    "conf": {
      "name": "OLED Compensation Failures",
      "state_topic": "tv/telemetry",
      "value_template": "{{ (value_json.get('oled') or {}).get('failure_alerts') if value_json.get('oled') and (value_json.get('oled') or {}).get('failure_alerts') is not none else 0 }}",
      "entity_category": "diagnostic",
      "icon": "mdi:alert-circle-outline",
      "unique_id": "lg_tv_oled_failure_alerts",
      "default_entity_id": "sensor.lg_tv_oled_failure_alerts",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'offline' if value == 'offline' else 'online' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "binary_sensor",
    "id": "oled_asbl_dimmer",
    "conf": {
      "name": "OLED ASBL Protection",
      "state_topic": "tv/telemetry",
      "value_template": "{{ \"OFF\" if value_json.get('tvOff') else (\"ON\" if value_json.get('oled') and (value_json.get('oled') or {}).get('asbl_protection') == \"Active\" else \"OFF\") }}",
      "entity_category": "diagnostic",
      "icon": "mdi:shield-check",
      "unique_id": "lg_tv_oled_asbl_dimmer",
      "default_entity_id": "binary_sensor.lg_tv_oled_asbl_dimmer",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'offline' if value == 'offline' else 'online' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "switch",
    "id": "pixel_refresher_schedule",
    "conf": {
      "name": "Schedule Pixel Refresher",
      "command_topic": "tv/command/refresher",
      "state_topic": "tv/telemetry",
      "value_template": "{{ 'ON' if value_json.get('oled') and (value_json.get('oled') or {}).get('refresher_status') == 'Scheduled' else 'OFF' }}",
      "payload_on": "schedule",
      "payload_off": "cancel",
      "icon": "mdi:television-shimmer",
      "unique_id": "lg_tv_pixel_refresher_schedule",
      "default_entity_id": "switch.lg_tv_pixel_refresher_schedule",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'online' if value in ['online', 'off'] else 'offline' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "select",
    "id": "picture_mode",
    "conf": {
      "name": "Picture Mode",
      "command_topic": "tv/command/picture_mode",
      "state_topic": "tv/telemetry",
      "icon": "mdi:image-filter-black-white",
      "options": [
        "HDR Game",
        "HDR Vivid"
      ],
      "command_template": "{{ {\"HDR Game\":\"hdrGame\",\"HDR Vivid\":\"hdrVivid\"}.get(value, value) }}",
      "value_template": "{{ {\"hdrGame\":\"HDR Game\",\"hdrVivid\":\"HDR Vivid\"}.get(((value_json.get('picture') or {}).get('mode_raw') if value_json.get('picture') else \"standard\"), \"None\") }}",
      "unique_id": "lg_tv_picture_mode",
      "default_entity_id": "select.lg_tv_picture_mode",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'online' if value in ['online', 'off'] else 'offline' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "select",
    "id": "energy_saving",
    "conf": {
      "name": "Energy Saving Step",
      "command_topic": "tv/command/energySaving",
      "state_topic": "tv/state/picture/energySaving",
      "options": [
        "auto",
        "off",
        "min",
        "med",
        "max",
        "screen_off"
      ],
      "icon": "mdi:brightness-auto",
      "unique_id": "lg_tv_energy_saving",
      "default_entity_id": "select.lg_tv_energy_saving",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'online' if value in ['online', 'off'] else 'offline' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "select",
    "id": "sound_output",
    "conf": {
      "name": "Sound Output",
      "command_topic": "tv/command/sound_output",
      "state_topic": "tv/telemetry",
      "icon": "mdi:speaker",
      "options": [
        "TV Speaker",
        "HDMI ARC",
        "Optical",
        "Headphone / AUX",
        "Bluetooth",
        "TV Speaker + Optical",
        "TV Speaker + Bluetooth",
        "TV Speaker + HDMI ARC",
        "LG WOWCAST",
        "Mobile Phone",
        "WiSA Speakers",
        "External Speaker",
        "Line Out",
        "LG Sound Sync",
        "TV Speaker + Headphone"
      ],
      "command_template": "{{ {\"TV Speaker\":\"tv_speaker\",\"HDMI ARC\":\"external_arc\",\"Optical\":\"optical\",\"Headphone / AUX\":\"headphone\",\"Bluetooth\":\"bt_soundbar\",\"TV Speaker + Optical\":\"tv_external_speaker\",\"TV Speaker + Bluetooth\":\"tv_speaker_bluetooth\",\"TV Speaker + HDMI ARC\":\"tv_speaker_external_arc\",\"LG WOWCAST\":\"wow_cast\",\"Mobile Phone\":\"mobile_phone\",\"WiSA Speakers\":\"wisa_speaker\",\"External Speaker\":\"external_speaker\",\"Line Out\":\"lineout\",\"LG Sound Sync\":\"soundbar\",\"TV Speaker + Headphone\":\"tv_speaker_headphone\"}.get(value, value) }}",
      "value_template": "{{ {\"tv_speaker\":\"TV Speaker\",\"external_arc\":\"HDMI ARC\",\"optical\":\"Optical\",\"external_optical\":\"Optical\",\"ext_speaker_optical\":\"Optical\",\"ext_speaker_builtin_lg_optical\":\"Optical\",\"ext_speaker_arc\":\"HDMI ARC\",\"headphone\":\"Headphone / AUX\",\"bt_soundbar\":\"Bluetooth\",\"tv_external_speaker\":\"TV Speaker + Optical\",\"tv_speaker_bluetooth\":\"TV Speaker + Bluetooth\",\"tv_speaker_bt_surround\":\"TV Speaker + Bluetooth\",\"tv_speaker_external_arc\":\"TV Speaker + HDMI ARC\",\"wow_cast\":\"LG WOWCAST\",\"mobile_phone\":\"Mobile Phone\",\"wisa_speaker\":\"WiSA Speakers\",\"external_speaker\":\"External Speaker\",\"lineout\":\"Line Out\",\"soundbar\":\"LG Sound Sync\",\"tv_speaker_headphone\":\"TV Speaker + Headphone\",\"internal\":\"TV Speaker\"}.get(((value_json.get('sound') or {}).get('output_raw') if value_json.get('sound') else \"tv_speaker\"), \"None\") }}",
      "unique_id": "lg_tv_sound_output",
      "default_entity_id": "select.lg_tv_sound_output",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'online' if value in ['online', 'off'] else 'offline' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "select",
    "id": "app",
    "conf": {
      "name": "Launch App",
      "command_topic": "tv/command/launch_app",
      "command_template": "{{ {\"YouTube\":\"youtube.leanback.v4\",\"Settings\":\"com.palm.app.settings\"}.get(value, value) }}",
      "state_topic": "tv/telemetry",
      "value_template": "{{ {\"youtube.leanback.v4\":\"YouTube\",\"com.palm.app.settings\":\"Settings\"}.get(value_json.get('app_id'), \"None\") }}",
      "json_attributes_topic": "tv/telemetry",
      "json_attributes_template": "{{ {\"app_ids\":{\"YouTube\":\"youtube.leanback.v4\",\"Settings\":\"com.palm.app.settings\"}} | tojson }}",
      "options": [
        "YouTube",
        "Settings"
      ],
      "icon": "mdi:apps",
      "unique_id": "lg_tv_app",
      "default_entity_id": "select.lg_tv_app",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'online' if value in ['online', 'off'] else 'offline' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "sensor",
    "id": "gpu_clock",
    "conf": {
      "name": "GPU Clock",
      "state_topic": "tv/telemetry",
      "value_template": "{{ value_json.get('gpuMhz') if value_json.get('gpuMhz') else none }}",
      "unit_of_measurement": "MHz",
      "state_class": "measurement",
      "icon": "mdi:expansion-card",
      "unique_id": "lg_tv_gpu_clock",
      "default_entity_id": "sensor.lg_tv_gpu_clock",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'online' if value in ['online', 'off'] else 'offline' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "sensor",
    "id": "panel_dimming",
    "conf": {
      "name": "Panel Dimming",
      "state_topic": "tv/telemetry",
      "value_template": "{{ \"Off\" if value_json.get('tvOff') else (value_json.get('dimming')) }}",
      "icon": "mdi:brightness-auto",
      "unique_id": "lg_tv_panel_dimming",
      "default_entity_id": "sensor.lg_tv_panel_dimming",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'offline' if value == 'offline' else 'online' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "sensor",
    "id": "app_storage_free",
    "conf": {
      "name": "App Storage Free",
      "state_topic": "tv/telemetry",
      "value_template": "{{ ((value_json.get('appStorage') or {}).get('freeMb') / 1024) | round(1) if value_json.get('appStorage') else none }}",
      "unit_of_measurement": "GB",
      "state_class": "measurement",
      "icon": "mdi:harddisk",
      "unique_id": "lg_tv_app_storage_free",
      "default_entity_id": "sensor.lg_tv_app_storage_free",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'offline' if value == 'offline' else 'online' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "sensor",
    "id": "ambient_light",
    "conf": {
      "name": "Ambient Light",
      "state_topic": "tv/telemetry",
      "value_template": "{{ (value_json.get('lightSensor') or {}).get('lux') if value_json.get('lightSensor') else none }}",
      "device_class": "illuminance",
      "state_class": "measurement",
      "icon": "mdi:brightness-5",
      "unique_id": "lg_tv_ambient_light",
      "default_entity_id": "sensor.lg_tv_ambient_light",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'online' if value in ['online', 'off'] else 'offline' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "select",
    "id": "sleep_timer",
    "conf": {
      "name": "Sleep Timer",
      "command_topic": "tv/command/sleepTimer",
      "state_topic": "tv/telemetry",
      "options": [
        "Off",
        "10 min",
        "30 min",
        "60 min",
        "90 min",
        "120 min"
      ],
      "command_template": "{{ {\"Off\":\"off\",\"10 min\":\"10\",\"30 min\":\"30\",\"60 min\":\"60\",\"90 min\":\"90\",\"120 min\":\"120\"}[value] }}",
      "value_template": "{{ {\"off\":\"Off\",\"10\":\"10 min\",\"30\":\"30 min\",\"60\":\"60 min\",\"90\":\"90 min\",\"120\":\"120 min\"}.get(value_json.get('sleepTimer'), \"Off\") }}",
      "icon": "mdi:timer-outline",
      "unique_id": "lg_tv_sleep_timer",
      "default_entity_id": "select.lg_tv_sleep_timer",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'online' if value in ['online', 'off'] else 'offline' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "switch",
    "id": "on_timer",
    "conf": {
      "name": "On Timer",
      "command_topic": "tv/command/powerTimer/on",
      "state_topic": "tv/telemetry",
      "json_attributes_topic": "tv/telemetry",
      "value_template": "{{ \"ON\" if ((value_json.get('powerTimers') or {}).get(\"on\") or {}).get(\"enabled\") else \"OFF\" }}",
      "json_attributes_template": "{% set t = (value_json.get('powerTimers') or {}).get(\"on\") %}{% set names = [\"Sunday\", \"Monday\", \"Tuesday\", \"Wednesday\", \"Thursday\", \"Friday\", \"Saturday\"] %}{% set ns = namespace(days=[]) %}{% for d in ((t.get(\"days\") or []) if t else []) %}{% set ns.days = ns.days + [names[d]] %}{% endfor %}{{ {\"time\": t.get(\"time\"), \"days\": ns.days} | tojson if t else \"{}\" }}",
      "payload_on": "on",
      "payload_off": "off",
      "state_on": "ON",
      "state_off": "OFF",
      "icon": "mdi:alarm",
      "unique_id": "lg_tv_on_timer",
      "default_entity_id": "switch.lg_tv_on_timer",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'online' if value in ['online', 'off'] else 'offline' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "switch",
    "id": "off_timer",
    "conf": {
      "name": "Off Timer",
      "command_topic": "tv/command/powerTimer/off",
      "state_topic": "tv/telemetry",
      "json_attributes_topic": "tv/telemetry",
      "value_template": "{{ \"ON\" if ((value_json.get('powerTimers') or {}).get(\"off\") or {}).get(\"enabled\") else \"OFF\" }}",
      "json_attributes_template": "{% set t = (value_json.get('powerTimers') or {}).get(\"off\") %}{% set names = [\"Sunday\", \"Monday\", \"Tuesday\", \"Wednesday\", \"Thursday\", \"Friday\", \"Saturday\"] %}{% set ns = namespace(days=[]) %}{% for d in ((t.get(\"days\") or []) if t else []) %}{% set ns.days = ns.days + [names[d]] %}{% endfor %}{{ {\"time\": t.get(\"time\"), \"days\": ns.days} | tojson if t else \"{}\" }}",
      "payload_on": "on",
      "payload_off": "off",
      "state_on": "ON",
      "state_off": "OFF",
      "icon": "mdi:alarm-off",
      "unique_id": "lg_tv_off_timer",
      "default_entity_id": "switch.lg_tv_off_timer",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'online' if value in ['online', 'off'] else 'offline' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "switch",
    "id": "standby_light",
    "conf": {
      "name": "Standby LED",
      "command_topic": "tv/command/standbyLight",
      "state_topic": "tv/telemetry",
      "value_template": "{{ \"ON\" if value_json.get('lights') and (value_json.get('lights') or {}).get('standby') else \"OFF\" }}",
      "payload_on": "on",
      "payload_off": "off",
      "state_on": "ON",
      "state_off": "OFF",
      "icon": "mdi:led-on",
      "unique_id": "lg_tv_standby_light",
      "default_entity_id": "switch.lg_tv_standby_light",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'online' if value in ['online', 'off'] else 'offline' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "switch",
    "id": "logo_light",
    "conf": {
      "name": "Logo Light",
      "command_topic": "tv/command/logoLight",
      "state_topic": "tv/telemetry",
      "value_template": "{{ \"ON\" if value_json.get('lights') and (value_json.get('lights') or {}).get('logo') else \"OFF\" }}",
      "payload_on": "on",
      "payload_off": "off",
      "state_on": "ON",
      "state_off": "OFF",
      "icon": "mdi:television-ambient-light",
      "unique_id": "lg_tv_logo_light",
      "default_entity_id": "switch.lg_tv_logo_light",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'online' if value in ['online', 'off'] else 'offline' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "button",
    "id": "screensaver",
    "conf": {
      "name": "Start Screensaver",
      "command_topic": "tv/command/screensaver",
      "payload_press": "press",
      "icon": "mdi:television-shimmer",
      "unique_id": "lg_tv_screensaver",
      "default_entity_id": "button.lg_tv_screensaver",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'online' if value in ['online', 'off'] else 'offline' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "binary_sensor",
    "id": "screen_saver_active",
    "conf": {
      "name": "Screen Saver",
      "state_topic": "tv/telemetry",
      "value_template": "{{ \"OFF\" if value_json.get('tvOff') else (\"ON\" if value_json.get('screenSaver') else \"OFF\") }}",
      "icon": "mdi:television-shimmer",
      "unique_id": "lg_tv_screen_saver_active",
      "default_entity_id": "binary_sensor.lg_tv_screen_saver_active",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'offline' if value == 'offline' else 'online' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "select",
    "id": "screensaver_mode",
    "conf": {
      "name": "Screen Saver",
      "command_topic": "tv/command/screensaverMode",
      "state_topic": "tv/telemetry",
      "options": [
        "LG default",
        "Clock",
        "Clock with date",
        "Black screen",
        "Starfield",
        "Fireworks",
        "Bokeh",
        "Harmonograph",
        "Filament",
        "Currents",
        "Veils",
        "Incense",
        "Murmuration",
        "Vortex",
        "Neo",
        "Tesseract",
        "Panel vitals"
      ],
      "command_template": "{{ {\"LG default\":\"stock\",\"Clock\":\"clock\",\"Clock with date\":\"clock-date\",\"Black screen\":\"black\",\"Starfield\":\"starfield\",\"Fireworks\":\"fireworks\",\"Bokeh\":\"bokeh\",\"Harmonograph\":\"harmonograph\",\"Filament\":\"filament\",\"Currents\":\"currents\",\"Veils\":\"veils\",\"Incense\":\"incense\",\"Murmuration\":\"murmuration\",\"Vortex\":\"vortex\",\"Neo\":\"neo\",\"Tesseract\":\"tesseract\",\"Panel vitals\":\"vitals\"}[value] }}",
      "value_template": "{{ {\"stock\":\"LG default\",\"clock\":\"Clock\",\"clock-date\":\"Clock with date\",\"black\":\"Black screen\",\"starfield\":\"Starfield\",\"fireworks\":\"Fireworks\",\"bokeh\":\"Bokeh\",\"harmonograph\":\"Harmonograph\",\"filament\":\"Filament\",\"currents\":\"Currents\",\"veils\":\"Veils\",\"incense\":\"Incense\",\"murmuration\":\"Murmuration\",\"vortex\":\"Vortex\",\"neo\":\"Neo\",\"tesseract\":\"Tesseract\",\"vitals\":\"Panel vitals\"}.get(value_json.get('screensaverMode'), \"LG default\") }}",
      "icon": "mdi:television-shimmer",
      "unique_id": "lg_tv_screensaver_mode",
      "default_entity_id": "select.lg_tv_screensaver_mode",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'online' if value in ['online', 'off'] else 'offline' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "switch",
    "id": "ad_blocker",
    "conf": {
      "name": "Ad & Telemetry Blocker",
      "command_topic": "tv/command/adblock",
      "state_topic": "tv/telemetry",
      "value_template": "{{ \"ON\" if value_json.get('privacy') and (value_json.get('privacy') or {}).get('adblock') and ((value_json.get('privacy') or {}).get('adblock') or {}).get('enabled') else \"OFF\" }}",
      "payload_on": "ads",
      "payload_off": "off",
      "state_on": "ON",
      "state_off": "OFF",
      "icon": "mdi:shield-check",
      "unique_id": "lg_tv_ad_blocker",
      "default_entity_id": "switch.lg_tv_ad_blocker",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'online' if value in ['online', 'off'] else 'offline' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "button",
    "id": "volume_up",
    "conf": {
      "name": "Volume Up",
      "command_topic": "tv/command/volumeStep",
      "payload_press": "1",
      "icon": "mdi:volume-plus",
      "availability": [
        {
          "topic": "tv/status",
          "value_template": "{{ 'online' if value in ['online', 'off'] else 'offline' }}"
        },
        {
          "topic": "tv/telemetry",
          "value_template": "{{ \"offline\" if value_json.get('volume_control') == \"none\" else \"online\" }}"
        }
      ],
      "unique_id": "lg_tv_volume_up",
      "default_entity_id": "button.lg_tv_volume_up",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_mode": "all"
    }
  },
  {
    "type": "button",
    "id": "volume_down",
    "conf": {
      "name": "Volume Down",
      "command_topic": "tv/command/volumeStep",
      "payload_press": "-1",
      "icon": "mdi:volume-minus",
      "availability": [
        {
          "topic": "tv/status",
          "value_template": "{{ 'online' if value in ['online', 'off'] else 'offline' }}"
        },
        {
          "topic": "tv/telemetry",
          "value_template": "{{ \"offline\" if value_json.get('volume_control') == \"none\" else \"online\" }}"
        }
      ],
      "unique_id": "lg_tv_volume_down",
      "default_entity_id": "button.lg_tv_volume_down",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_mode": "all"
    }
  },
  {
    "type": "button",
    "id": "remote_up",
    "conf": {
      "name": "Remote Up",
      "command_topic": "tv/command/rcu",
      "payload_press": "up",
      "icon": "mdi:chevron-up",
      "unique_id": "lg_tv_remote_up",
      "default_entity_id": "button.lg_tv_remote_up",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'online' if value in ['online', 'off'] else 'offline' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "button",
    "id": "remote_down",
    "conf": {
      "name": "Remote Down",
      "command_topic": "tv/command/rcu",
      "payload_press": "down",
      "icon": "mdi:chevron-down",
      "unique_id": "lg_tv_remote_down",
      "default_entity_id": "button.lg_tv_remote_down",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'online' if value in ['online', 'off'] else 'offline' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "button",
    "id": "remote_left",
    "conf": {
      "name": "Remote Left",
      "command_topic": "tv/command/rcu",
      "payload_press": "left",
      "icon": "mdi:chevron-left",
      "unique_id": "lg_tv_remote_left",
      "default_entity_id": "button.lg_tv_remote_left",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'online' if value in ['online', 'off'] else 'offline' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "button",
    "id": "remote_right",
    "conf": {
      "name": "Remote Right",
      "command_topic": "tv/command/rcu",
      "payload_press": "right",
      "icon": "mdi:chevron-right",
      "unique_id": "lg_tv_remote_right",
      "default_entity_id": "button.lg_tv_remote_right",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'online' if value in ['online', 'off'] else 'offline' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "button",
    "id": "remote_ok",
    "conf": {
      "name": "Remote OK",
      "command_topic": "tv/command/rcu",
      "payload_press": "ok",
      "icon": "mdi:circle-medium",
      "unique_id": "lg_tv_remote_ok",
      "default_entity_id": "button.lg_tv_remote_ok",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'online' if value in ['online', 'off'] else 'offline' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "button",
    "id": "remote_back",
    "conf": {
      "name": "Remote Back",
      "command_topic": "tv/command/rcu",
      "payload_press": "back",
      "icon": "mdi:keyboard-return",
      "unique_id": "lg_tv_remote_back",
      "default_entity_id": "button.lg_tv_remote_back",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'online' if value in ['online', 'off'] else 'offline' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "button",
    "id": "remote_home",
    "conf": {
      "name": "Remote Home",
      "command_topic": "tv/command/rcu",
      "payload_press": "home",
      "icon": "mdi:home",
      "unique_id": "lg_tv_remote_home",
      "default_entity_id": "button.lg_tv_remote_home",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'online' if value in ['online', 'off'] else 'offline' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "event",
    "id": "remote_button",
    "conf": {
      "name": "Remote Button",
      "state_topic": "tv/events/button",
      "event_types": [
        "red",
        "green",
        "yellow",
        "blue"
      ],
      "icon": "mdi:remote",
      "unique_id": "lg_tv_remote_button",
      "default_entity_id": "event.lg_tv_remote_button",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'online' if value in ['online', 'off'] else 'offline' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "button",
    "id": "play",
    "conf": {
      "name": "Play",
      "command_topic": "tv/command/playback",
      "payload_press": "play",
      "icon": "mdi:play",
      "unique_id": "lg_tv_play",
      "default_entity_id": "button.lg_tv_play",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'online' if value in ['online', 'off'] else 'offline' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "button",
    "id": "pause",
    "conf": {
      "name": "Pause",
      "command_topic": "tv/command/playback",
      "payload_press": "pause",
      "icon": "mdi:pause",
      "unique_id": "lg_tv_pause",
      "default_entity_id": "button.lg_tv_pause",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'online' if value in ['online', 'off'] else 'offline' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "button",
    "id": "play_pause",
    "conf": {
      "name": "Play / Pause",
      "command_topic": "tv/command/playback",
      "payload_press": "playPause",
      "icon": "mdi:play-pause",
      "unique_id": "lg_tv_play_pause",
      "default_entity_id": "button.lg_tv_play_pause",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'online' if value in ['online', 'off'] else 'offline' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "button",
    "id": "stop",
    "conf": {
      "name": "Stop",
      "command_topic": "tv/command/playback",
      "payload_press": "stop",
      "icon": "mdi:stop",
      "unique_id": "lg_tv_stop",
      "default_entity_id": "button.lg_tv_stop",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'online' if value in ['online', 'off'] else 'offline' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "button",
    "id": "rewind",
    "conf": {
      "name": "Rewind",
      "command_topic": "tv/command/playback",
      "payload_press": "rewind",
      "icon": "mdi:rewind",
      "unique_id": "lg_tv_rewind",
      "default_entity_id": "button.lg_tv_rewind",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'online' if value in ['online', 'off'] else 'offline' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "button",
    "id": "fast_forward",
    "conf": {
      "name": "Fast Forward",
      "command_topic": "tv/command/playback",
      "payload_press": "fastForward",
      "icon": "mdi:fast-forward",
      "unique_id": "lg_tv_fast_forward",
      "default_entity_id": "button.lg_tv_fast_forward",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'online' if value in ['online', 'off'] else 'offline' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "button",
    "id": "restart",
    "conf": {
      "name": "Restart TV",
      "command_topic": "tv/command/reboot",
      "device_class": "restart",
      "icon": "mdi:restart",
      "unique_id": "lg_tv_restart",
      "default_entity_id": "button.lg_tv_restart",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'online' if value in ['online', 'off'] else 'offline' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "button",
    "id": "power_off",
    "conf": {
      "name": "Power Off TV",
      "command_topic": "tv/command/powerOff",
      "icon": "mdi:power",
      "unique_id": "lg_tv_power_off",
      "default_entity_id": "button.lg_tv_power_off",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'online' if value in ['online', 'off'] else 'offline' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "button",
    "id": "power_on",
    "conf": {
      "name": "Power On TV",
      "command_topic": "tv/command/powerOn",
      "icon": "mdi:power",
      "unique_id": "lg_tv_power_on",
      "default_entity_id": "button.lg_tv_power_on",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'online' if value in ['online', 'off'] else 'offline' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "switch",
    "id": "piccap",
    "conf": {
      "name": "PicCap Capture",
      "command_topic": "tv/command/piccap/power",
      "state_topic": "tv/state/piccap/power",
      "payload_on": "ON",
      "payload_off": "OFF",
      "icon": "mdi:television-ambient-light",
      "unique_id": "lg_tv_piccap",
      "default_entity_id": "switch.lg_tv_piccap",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'online' if value in ['online', 'off'] else 'offline' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "binary_sensor",
    "id": "power",
    "conf": {
      "name": "Power",
      "state_topic": "tv/telemetry",
      "value_template": "{{ \"OFF\" if value_json.get('tvOff') else \"ON\" }}",
      "device_class": "power",
      "unique_id": "lg_tv_power",
      "default_entity_id": "binary_sensor.lg_tv_power",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'offline' if value == 'offline' else 'online' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "select",
    "id": "sound_mode",
    "conf": {
      "name": "Sound Mode",
      "command_topic": "tv/command/lgs/soundMode",
      "state_topic": "tv/telemetry",
      "icon": "mdi:tune-vertical",
      "options": [
        "AI Sound Pro",
        "AI Sound",
        "Standard",
        "Clear Voice",
        "Cinema",
        "Sports",
        "Music",
        "Game Optimizer"
      ],
      "command_template": "{{ {\"AI Sound Pro\":\"aiSoundPlus\",\"AI Sound\":\"aiSound\",\"Standard\":\"standard\",\"Clear Voice\":\"news\",\"Cinema\":\"movie\",\"Sports\":\"sports\",\"Music\":\"music\",\"Game Optimizer\":\"game\"}.get(value, value) }}",
      "value_template": "{{ {\"aiSoundPlus\":\"AI Sound Pro\",\"aiSound\":\"AI Sound\",\"standard\":\"Standard\",\"news\":\"Clear Voice\",\"movie\":\"Cinema\",\"sports\":\"Sports\",\"music\":\"Music\",\"game\":\"Game Optimizer\"}.get((value_json.get('lgs') or {}).get(\"soundMode\"), \"None\") }}",
      "unique_id": "lg_tv_sound_mode",
      "default_entity_id": "select.lg_tv_sound_mode",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'online' if value in ['online', 'off'] else 'offline' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "switch",
    "id": "bluetooth_speaker_mode",
    "conf": {
      "name": "Bluetooth Speaker Mode",
      "command_topic": "tv/command/lgs/btSpeakerMode",
      "state_topic": "tv/telemetry",
      "icon": "mdi:bluetooth-audio",
      "value_template": "{{ \"ON\" if (value_json.get('lgs') or {}).get(\"btSpeakerMode\") else \"OFF\" }}",
      "payload_on": "ON",
      "payload_off": "OFF",
      "unique_id": "lg_tv_bluetooth_speaker_mode",
      "default_entity_id": "switch.lg_tv_bluetooth_speaker_mode",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'online' if value in ['online', 'off'] else 'offline' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "switch",
    "id": "simplink",
    "conf": {
      "name": "SIMPLINK (HDMI-CEC)",
      "command_topic": "tv/command/lgs/simplinkEnable",
      "state_topic": "tv/telemetry",
      "icon": "mdi:link-variant",
      "value_template": "{{ \"ON\" if (value_json.get('lgs') or {}).get(\"simplinkEnable\") else \"OFF\" }}",
      "payload_on": "ON",
      "payload_off": "OFF",
      "unique_id": "lg_tv_simplink",
      "default_entity_id": "switch.lg_tv_simplink",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'online' if value in ['online', 'off'] else 'offline' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "switch",
    "id": "auto_power_sync",
    "conf": {
      "name": "Auto Power Sync",
      "command_topic": "tv/command/lgs/simplinkAutoPowerOn",
      "state_topic": "tv/telemetry",
      "icon": "mdi:power-plug",
      "value_template": "{{ \"ON\" if (value_json.get('lgs') or {}).get(\"simplinkAutoPowerOn\") else \"OFF\" }}",
      "payload_on": "ON",
      "payload_off": "OFF",
      "unique_id": "lg_tv_auto_power_sync",
      "default_entity_id": "switch.lg_tv_auto_power_sync",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'online' if value in ['online', 'off'] else 'offline' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "select",
    "id": "game_genre",
    "conf": {
      "name": "Game Genre",
      "command_topic": "tv/command/lgs/gameGenre",
      "state_topic": "tv/telemetry",
      "icon": "mdi:gamepad-variant",
      "options": [
        "Standard",
        "First-person shooter",
        "Role-playing",
        "Real-time strategy",
        "Sports",
        "User"
      ],
      "command_template": "{{ {\"Standard\":\"Standard\",\"First-person shooter\":\"FPS\",\"Role-playing\":\"RPG\",\"Real-time strategy\":\"RTS\",\"Sports\":\"Sports\",\"User\":\"USER\"}.get(value, value) }}",
      "value_template": "{{ {\"Standard\":\"Standard\",\"FPS\":\"First-person shooter\",\"RPG\":\"Role-playing\",\"RTS\":\"Real-time strategy\",\"Sports\":\"Sports\",\"USER\":\"User\"}.get((value_json.get('lgs') or {}).get(\"gameGenre\"), \"None\") }}",
      "unique_id": "lg_tv_game_genre",
      "default_entity_id": "select.lg_tv_game_genre",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'online' if value in ['online', 'off'] else 'offline' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "select",
    "id": "prevent_input_delay",
    "conf": {
      "name": "Prevent Input Delay",
      "command_topic": "tv/command/lgs/inputOptimization",
      "state_topic": "tv/telemetry",
      "icon": "mdi:timer-outline",
      "options": [
        "Standard",
        "Boost"
      ],
      "command_template": "{{ {\"Standard\":\"auto\",\"Boost\":\"on\"}.get(value, value) }}",
      "value_template": "{{ {\"auto\":\"Standard\",\"on\":\"Boost\"}.get((value_json.get('lgs') or {}).get(\"inputOptimization\"), \"None\") }}",
      "unique_id": "lg_tv_prevent_input_delay",
      "default_entity_id": "select.lg_tv_prevent_input_delay",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'online' if value in ['online', 'off'] else 'offline' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "switch",
    "id": "allm_setting",
    "conf": {
      "name": "ALLM",
      "command_topic": "tv/command/lgs/enableALLM",
      "state_topic": "tv/telemetry",
      "icon": "mdi:gamepad-square",
      "value_template": "{{ \"ON\" if (value_json.get('lgs') or {}).get(\"enableALLM\") else \"OFF\" }}",
      "payload_on": "ON",
      "payload_off": "OFF",
      "unique_id": "lg_tv_allm_setting",
      "default_entity_id": "switch.lg_tv_allm_setting",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'online' if value in ['online', 'off'] else 'offline' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "select",
    "id": "digital_sound_output",
    "conf": {
      "name": "Digital Sound Output",
      "command_topic": "tv/command/lgs/soundOutputDigital",
      "state_topic": "tv/telemetry",
      "icon": "mdi:surround-sound",
      "enabled_by_default": false,
      "entity_category": "config",
      "options": [
        "Auto",
        "PCM",
        "Pass Through"
      ],
      "command_template": "{{ {\"Auto\":\"auto\",\"PCM\":\"pcm\",\"Pass Through\":\"passThrough\"}.get(value, value) }}",
      "value_template": "{{ {\"auto\":\"Auto\",\"pcm\":\"PCM\",\"passThrough\":\"Pass Through\"}.get((value_json.get('lgs') or {}).get(\"soundOutputDigital\"), \"None\") }}",
      "unique_id": "lg_tv_digital_sound_output",
      "default_entity_id": "select.lg_tv_digital_sound_output",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'online' if value in ['online', 'off'] else 'offline' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "number",
    "id": "balance",
    "conf": {
      "name": "Balance",
      "command_topic": "tv/command/lgs/audioBalance",
      "state_topic": "tv/telemetry",
      "icon": "mdi:scale-balance",
      "enabled_by_default": false,
      "entity_category": "config",
      "value_template": "{{ (value_json.get('lgs') or {}).get(\"audioBalance\") }}",
      "min": -50,
      "max": 50,
      "step": 1,
      "mode": "slider",
      "unique_id": "lg_tv_balance",
      "default_entity_id": "number.lg_tv_balance",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'online' if value in ['online', 'off'] else 'offline' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "switch",
    "id": "automatic_volume",
    "conf": {
      "name": "Automatic Volume",
      "command_topic": "tv/command/lgs/autoVolume",
      "state_topic": "tv/telemetry",
      "icon": "mdi:volume-equal",
      "enabled_by_default": false,
      "entity_category": "config",
      "value_template": "{{ \"ON\" if (value_json.get('lgs') or {}).get(\"autoVolume\") else \"OFF\" }}",
      "payload_on": "ON",
      "payload_off": "OFF",
      "unique_id": "lg_tv_automatic_volume",
      "default_entity_id": "switch.lg_tv_automatic_volume",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'online' if value in ['online', 'off'] else 'offline' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "switch",
    "id": "earc",
    "conf": {
      "name": "eARC",
      "command_topic": "tv/command/lgs/eArcSupport",
      "state_topic": "tv/telemetry",
      "icon": "mdi:audio-input-rca",
      "enabled_by_default": false,
      "entity_category": "config",
      "value_template": "{{ \"ON\" if (value_json.get('lgs') or {}).get(\"eArcSupport\") else \"OFF\" }}",
      "payload_on": "ON",
      "payload_off": "OFF",
      "unique_id": "lg_tv_earc",
      "default_entity_id": "switch.lg_tv_earc",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'online' if value in ['online', 'off'] else 'offline' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "switch",
    "id": "ip_control",
    "conf": {
      "name": "IP Control",
      "command_topic": "tv/command/lgs/ipControl",
      "state_topic": "tv/telemetry",
      "icon": "mdi:lan",
      "enabled_by_default": false,
      "entity_category": "config",
      "value_template": "{{ \"ON\" if (value_json.get('lgs') or {}).get(\"ipControl\") else \"OFF\" }}",
      "payload_on": "ON",
      "payload_off": "OFF",
      "unique_id": "lg_tv_ip_control",
      "default_entity_id": "switch.lg_tv_ip_control",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'online' if value in ['online', 'off'] else 'offline' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "switch",
    "id": "vrr_gsync",
    "conf": {
      "name": "VRR & G-Sync",
      "command_topic": "tv/command/lgs/gameOptimization",
      "state_topic": "tv/telemetry",
      "icon": "mdi:sync",
      "enabled_by_default": false,
      "entity_category": "config",
      "value_template": "{{ \"ON\" if (value_json.get('lgs') or {}).get(\"gameOptimization\") else \"OFF\" }}",
      "payload_on": "ON",
      "payload_off": "OFF",
      "unique_id": "lg_tv_vrr_gsync",
      "default_entity_id": "switch.lg_tv_vrr_gsync",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'online' if value in ['online', 'off'] else 'offline' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "switch",
    "id": "freesync_premium",
    "conf": {
      "name": "AMD FreeSync Premium",
      "command_topic": "tv/command/lgs/freesync",
      "state_topic": "tv/telemetry",
      "icon": "mdi:sync",
      "enabled_by_default": false,
      "entity_category": "config",
      "value_template": "{{ \"ON\" if (value_json.get('lgs') or {}).get(\"freesync\") else \"OFF\" }}",
      "payload_on": "ON",
      "payload_off": "OFF",
      "unique_id": "lg_tv_freesync_premium",
      "default_entity_id": "switch.lg_tv_freesync_premium",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'online' if value in ['online', 'off'] else 'offline' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "select",
    "id": "dark_room_mode",
    "conf": {
      "name": "Dark Room Mode",
      "command_topic": "tv/command/lgs/darkMode",
      "state_topic": "tv/telemetry",
      "icon": "mdi:weather-night",
      "enabled_by_default": false,
      "entity_category": "config",
      "options": [
        "Off",
        "Level 1",
        "Level 2"
      ],
      "command_template": "{{ {\"Off\":\"off\",\"Level 1\":\"level1\",\"Level 2\":\"level2\"}.get(value, value) }}",
      "value_template": "{{ {\"off\":\"Off\",\"level1\":\"Level 1\",\"level2\":\"Level 2\"}.get((value_json.get('lgs') or {}).get(\"darkMode\"), \"None\") }}",
      "unique_id": "lg_tv_dark_room_mode",
      "default_entity_id": "select.lg_tv_dark_room_mode",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'online' if value in ['online', 'off'] else 'offline' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "number",
    "id": "black_stabilizer",
    "conf": {
      "name": "Black Stabilizer",
      "command_topic": "tv/command/lgs/blackStabilizer",
      "state_topic": "tv/telemetry",
      "icon": "mdi:brightness-4",
      "enabled_by_default": false,
      "entity_category": "config",
      "value_template": "{{ (value_json.get('lgs') or {}).get(\"blackStabilizer\") }}",
      "min": 0,
      "max": 20,
      "step": 1,
      "mode": "slider",
      "unique_id": "lg_tv_black_stabilizer",
      "default_entity_id": "number.lg_tv_black_stabilizer",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'online' if value in ['online', 'off'] else 'offline' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "number",
    "id": "white_stabilizer",
    "conf": {
      "name": "White Stabilizer",
      "command_topic": "tv/command/lgs/whiteStabilizer",
      "state_topic": "tv/telemetry",
      "icon": "mdi:brightness-5",
      "enabled_by_default": false,
      "entity_category": "config",
      "value_template": "{{ (value_json.get('lgs') or {}).get(\"whiteStabilizer\") }}",
      "min": 0,
      "max": 20,
      "step": 1,
      "mode": "slider",
      "unique_id": "lg_tv_white_stabilizer",
      "default_entity_id": "number.lg_tv_white_stabilizer",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'online' if value in ['online', 'off'] else 'offline' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "switch",
    "id": "ai_game_sound",
    "conf": {
      "name": "AI Game Sound",
      "command_topic": "tv/command/lgs/aigamesound",
      "state_topic": "tv/telemetry",
      "icon": "mdi:speaker-wireless",
      "enabled_by_default": false,
      "entity_category": "config",
      "value_template": "{{ \"ON\" if (value_json.get('lgs') or {}).get(\"aigamesound\") else \"OFF\" }}",
      "payload_on": "ON",
      "payload_off": "OFF",
      "unique_id": "lg_tv_ai_game_sound",
      "default_entity_id": "switch.lg_tv_ai_game_sound",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'online' if value in ['online', 'off'] else 'offline' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "switch",
    "id": "screen_saver_ads",
    "conf": {
      "name": "Screen Saver Ads",
      "command_topic": "tv/command/lgs/screenSaverAd",
      "state_topic": "tv/telemetry",
      "icon": "mdi:advertisements",
      "enabled_by_default": false,
      "entity_category": "config",
      "value_template": "{{ \"ON\" if (value_json.get('lgs') or {}).get(\"screenSaverAd\") else \"OFF\" }}",
      "payload_on": "ON",
      "payload_off": "OFF",
      "unique_id": "lg_tv_screen_saver_ads",
      "default_entity_id": "switch.lg_tv_screen_saver_ads",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'online' if value in ['online', 'off'] else 'offline' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "switch",
    "id": "home_sponsored_tiles",
    "conf": {
      "name": "Sponsored Tiles on Home",
      "command_topic": "tv/command/lgs/homePromotion",
      "state_topic": "tv/telemetry",
      "icon": "mdi:advertisements",
      "enabled_by_default": false,
      "entity_category": "config",
      "value_template": "{{ \"ON\" if (value_json.get('lgs') or {}).get(\"homePromotion\") else \"OFF\" }}",
      "payload_on": "ON",
      "payload_off": "OFF",
      "unique_id": "lg_tv_home_sponsored_tiles",
      "default_entity_id": "switch.lg_tv_home_sponsored_tiles",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'online' if value in ['online', 'off'] else 'offline' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "switch",
    "id": "home_recommendations",
    "conf": {
      "name": "Recommendations on Home",
      "command_topic": "tv/command/lgs/contentRecommendation",
      "state_topic": "tv/telemetry",
      "icon": "mdi:advertisements",
      "enabled_by_default": false,
      "entity_category": "config",
      "value_template": "{{ \"ON\" if (value_json.get('lgs') or {}).get(\"contentRecommendation\") else \"OFF\" }}",
      "payload_on": "ON",
      "payload_off": "OFF",
      "unique_id": "lg_tv_home_recommendations",
      "default_entity_id": "switch.lg_tv_home_recommendations",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'online' if value in ['online', 'off'] else 'offline' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "switch",
    "id": "ads_while_watching",
    "conf": {
      "name": "Ads While Watching",
      "command_topic": "tv/command/lgs/livePromotion",
      "state_topic": "tv/telemetry",
      "icon": "mdi:advertisements",
      "enabled_by_default": false,
      "entity_category": "config",
      "value_template": "{{ \"ON\" if (value_json.get('lgs') or {}).get(\"livePromotion\") else \"OFF\" }}",
      "payload_on": "ON",
      "payload_off": "OFF",
      "unique_id": "lg_tv_ads_while_watching",
      "default_entity_id": "switch.lg_tv_ads_while_watching",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'online' if value in ['online', 'off'] else 'offline' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "switch",
    "id": "smart_tips",
    "conf": {
      "name": "Smart Tips",
      "command_topic": "tv/command/lgs/aiNudge",
      "state_topic": "tv/telemetry",
      "icon": "mdi:lightbulb-outline",
      "enabled_by_default": false,
      "entity_category": "config",
      "value_template": "{{ \"ON\" if (value_json.get('lgs') or {}).get(\"aiNudge\") else \"OFF\" }}",
      "payload_on": "ON",
      "payload_off": "OFF",
      "unique_id": "lg_tv_smart_tips",
      "default_entity_id": "switch.lg_tv_smart_tips",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'online' if value in ['online', 'off'] else 'offline' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  },
  {
    "type": "switch",
    "id": "smart_tips_in_settings",
    "conf": {
      "name": "Smart Tips in Settings",
      "command_topic": "tv/command/lgs/aiSettingsNudge",
      "state_topic": "tv/telemetry",
      "icon": "mdi:lightbulb-outline",
      "enabled_by_default": false,
      "entity_category": "config",
      "value_template": "{{ \"ON\" if (value_json.get('lgs') or {}).get(\"aiSettingsNudge\") else \"OFF\" }}",
      "payload_on": "ON",
      "payload_off": "OFF",
      "unique_id": "lg_tv_smart_tips_in_settings",
      "default_entity_id": "switch.lg_tv_smart_tips_in_settings",
      "device": {
        "identifiers": [
          "lg_tv"
        ],
        "name": "LG webOS TV",
        "model": "webOS TV",
        "manufacturer": "LG",
        "sw_version": "webOS (tvweb)"
      },
      "availability_topic": "tv/status",
      "availability_template": "{{ 'online' if value in ['online', 'off'] else 'offline' }}",
      "payload_available": "online",
      "payload_not_available": "offline"
    }
  }
])GHJSON");
    return value;
}
}

Json haDiscoveryTemplates() { return templates(); }

} } // namespace nativetv::daemon
