#pragma once

// Suppress C++17 deprecation of std::codecvt used in F4SEMenuFramework.h
#define _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING
#define _AMD64_

#pragma warning(push)
#include "F4SE/F4SE.h"
#include "RE/Fallout.h"
#include "REX/REX.h"
#pragma warning(pop)

#pragma warning(disable: 4100)
#pragma warning(disable: 4189)
#pragma warning(disable: 4244)
#pragma warning(disable: 4302)
#pragma warning(disable: 4311)

#define DLLEXPORT __declspec(dllexport)

using namespace std::literals;

// Standard library headers used throughout
#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <format>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <source_location>
#include <string>
#include <thread>
#include <utility>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Keep the existing logger::level(...) call sites while using the
// multi-runtime CommonLib logging backend configured by F4SE::Init.
namespace logger
{
	inline void trace(std::string_view a_message)
	{
		REX::Impl::Log(std::source_location::current(), REX::ELogLevel::Trace, a_message);
	}

	inline void debug(std::string_view a_message)
	{
		REX::Impl::Log(std::source_location::current(), REX::ELogLevel::Debug, a_message);
	}

	inline void info(std::string_view a_message)
	{
		REX::Impl::Log(std::source_location::current(), REX::ELogLevel::Info, a_message);
	}

	inline void warn(std::string_view a_message)
	{
		REX::Impl::Log(std::source_location::current(), REX::ELogLevel::Warning, a_message);
	}

	inline void error(std::string_view a_message)
	{
		REX::Impl::Log(std::source_location::current(), REX::ELogLevel::Error, a_message);
	}

	inline void critical(std::string_view a_message)
	{
		REX::Impl::Log(std::source_location::current(), REX::ELogLevel::Critical, a_message);
	}

	template <class... Args>
	void trace(std::format_string<Args...> a_format, Args&&... a_args)
	{
		REX::Impl::Log(
			std::source_location::current(),
			REX::ELogLevel::Trace,
			a_format,
			std::forward<Args>(a_args)...);
	}

	template <class... Args>
	void debug(std::format_string<Args...> a_format, Args&&... a_args)
	{
		REX::Impl::Log(
			std::source_location::current(),
			REX::ELogLevel::Debug,
			a_format,
			std::forward<Args>(a_args)...);
	}

	template <class... Args>
	void info(std::format_string<Args...> a_format, Args&&... a_args)
	{
		REX::Impl::Log(
			std::source_location::current(),
			REX::ELogLevel::Info,
			a_format,
			std::forward<Args>(a_args)...);
	}

	template <class... Args>
	void warn(std::format_string<Args...> a_format, Args&&... a_args)
	{
		REX::Impl::Log(
			std::source_location::current(),
			REX::ELogLevel::Warning,
			a_format,
			std::forward<Args>(a_args)...);
	}

	template <class... Args>
	void error(std::format_string<Args...> a_format, Args&&... a_args)
	{
		REX::Impl::Log(
			std::source_location::current(),
			REX::ELogLevel::Error,
			a_format,
			std::forward<Args>(a_args)...);
	}

	template <class... Args>
	void critical(std::format_string<Args...> a_format, Args&&... a_args)
	{
		REX::Impl::Log(
			std::source_location::current(),
			REX::ELogLevel::Critical,
			a_format,
			std::forward<Args>(a_args)...);
	}
}

// SimpleIni for INI file parsing
#include "SimpleIni.h"
