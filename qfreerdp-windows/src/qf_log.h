#pragma once

#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>

#include <cstdlib>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

namespace qf::log {

inline constexpr int kFormatWidth = 20;

inline std::once_flag& init_once_flag()
{
    static std::once_flag flag;
    return flag;
}

inline void init()
{
    std::call_once(init_once_flag(), [] {
        // Default level: info (shows connection, channel events, etc.)
        // Override with SPDLOG_LEVEL env var for runtime control, e.g.:
        //   set SPDLOG_LEVEL=debug
        spdlog::set_level(spdlog::level::info);
        spdlog::flush_on(spdlog::level::info);
    });
}

inline spdlog::level::level_enum to_level(spdlog::level::level_enum level)
{
    init();
    return level;
}

template <typename... Args>
inline void write(spdlog::level::level_enum level, std::string_view action,
                  fmt::format_string<Args...> format, Args&&... args)
{
    const auto message = fmt::format(format, std::forward<Args>(args)...);
    spdlog::log(level, "{:<22} {}", std::format("[{}]", action), message);
}

template <typename... Args>
inline void info(std::string_view action, fmt::format_string<Args...> format, Args&&... args)
{
    write(spdlog::level::info, action, format, std::forward<Args>(args)...);
}

template <typename... Args>
inline void warn(std::string_view action, fmt::format_string<Args...> format, Args&&... args)
{
    write(spdlog::level::warn, action, format, std::forward<Args>(args)...);
}

template <typename... Args>
inline void error(std::string_view action, fmt::format_string<Args...> format, Args&&... args)
{
    write(spdlog::level::err, action, format, std::forward<Args>(args)...);
}

} // namespace qf::log
