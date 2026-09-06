/**
 * @file local_config.example.h
 * @brief 本機設定範本(WiFi 帳密、Tailnet 名稱)
 *
 * 使用方式:
 *   cp examples/basic_connect/main/local_config.example.h \
 *      examples/basic_connect/main/local_config.h
 *   然後編輯 local_config.h 填入真實數值。
 *
 * local_config.h 已被 .gitignore 排除。main.c 會優先 include 它,
 * 找不到就退回用這個範本(乾淨的 clone 也能編譯,只是連不上你的網路)。
 */

#pragma once

/* Tailnet 名稱(通常是註冊 Tailscale 用的 email) */
#define TAILSCALE_TAILNET   "you@example.com"

/* 韌體內建的 WiFi 清單,格式:{ "SSID", "密碼" },  數量不限 */
#define WIFI_CRED_LIST \
    { "YourSSID",     "YourPassword" }, \
    { "YourSSID_5G",  "YourPassword" },
