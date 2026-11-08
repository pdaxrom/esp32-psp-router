# История диагностики WPA2, 8 ноября 2026

**Результат:** после полного перезапуска PSP пользователь подтвердил работу.
Снимок `dist/wpa2-after-psp-reboot.json` показывает два успешных обмена
M1 → M2 → M3 → M4 и JOIN при штатном внешнем EAPOL v2.
PSP передаёт M2/M4 с EAPOL v1, что IDF принимает штатно. Изменение версии
заголовка ESP не потребовалось. Диагностические маршруты и перехватчики
удаляются из обычного релиза 2.1.1.

Ниже — сохранённый отчёт с последовательностью исследования и промежуточными
неудачными проверками; они не описывают итоговое состояние WPA2.

# PSP Router 2.1.0 — отчёт о проверках

Дата: **8 ноября 2026**, Asia/Yerevan.

## Текущий статус

Добавлен **WPA2-PSK / AES-CCMP по умолчанию** для PSP с включённым WPA2 Support
в ARK-5. Legacy WPA1/TKIP и открытый диагностический режим выбираются явно при
сборке. Автоматического переключения защиты нет.

**ECM/WPA2/router 2.1.0 прошит и запущен.** Esptool 5.6.0 проверил хэши
bootloader/partition/app и завершился кодом 0. USB WAN получил 192.168.2.6/24,
шлюз/DNS 192.168.2.1; LAN — 192.168.50.1/24. Проверка AP authmode/cipher/PMF/protocol
при запуске прошла, полный DNS/HTTPS live-прогон завершился успешно.
**Проверка WPA2 с PSP/ARK-5 не прошла:** пользователь сообщил цикл
«подключение / обмен ключами» до таймаута. Успешные сборки, AP readback и USB
DNS/HTTPS не подтверждают совместимость Wi-Fi handshake. Идёт диагностика.
Рабочая WPA1 2.0.1 зафиксирована отдельно в коммите `350a84a`.

Пользователь подтвердил открытие zond.am и pdaxrom.org на прежнем WPA1 2.0.1.
Аппаратные результаты 2.0.1 сохранены в [архивном отчёте](TEST_REPORT_V2_0_1.md)
и не заменяют проверку WPA2 после обновления.

## Совместимость WPA2

Изучен [код WPA2 ARK-5](https://github.com/PSP-Arkfive/ARK-5/blob/28eec331e46445b101f769161a3c4c148e9d1c14/Compat/PSP/src/wpa2.c),
commit `28eec331e46445b101f769161a3c4c148e9d1c14`. Его RSN information объявляет
CCMP для группового и парного ключа, AKM PSK; патч переключает обмен на RSN.
Это источник требований совместимости, не зависимость прошивки ESP.

На ESP: WIFI_AUTH_WPA2_PSK, WIFI_CIPHER_TYPE_CCMP, PMF отключён, SSID PSP-NET,
2,4 ГГц / 802.11b / канал 6. После установки конфигурации проверяются authmode,
pairwise cipher, PMF, канал, protocol и видимость SSID.
WPA2 использует штатный аутентификатор IDF с EAPOL v2; linker-wrapper с EAPOL v1
компилируется и привязывается **только** для явно выбранного WPA1.

На PSP нужно включить WPA2 Support, перезапустить консоль и создать подключение
с AES, пароль `psp3000wifi`. Для явной сборки WPA1/TKIP WPA2 Support выключается.

## Среда и память

ESP-IDF v6.1-dev-4811-gfce70533fa, commit
`fce70533fa1fdeef9f903ce5e87ff8d292be992b`; GCC 15.2.0;
esp_tinyusb 2.0.1~1, tinyusb 0.21.0~4; registry-хэши в dependencies.lock.
Python 3.15.2, Component Manager 3.1.2, esp-idf-kconfig 3.14.0, esptool 5.6.0.
Новых внешних зависимостей нет; IDF и managed-компоненты не патчились.

ESP32-S3: CPU 240 МГц для router, 4 МиБ Flash QIO/80 МГц, 2 МиБ Quad PSRAM/40 МГц.
Проверка фактических 4194304/2097152 байт до запуска AP сохранена. Native USB-C:
GPIO19 D−, GPIO20 D+. Нет USB-консоли/трассировки, eFuse и защита загрузчика
не изменяются; восстановление через BOOT/RESET остаётся доступным.

## Выполненные проверки 2.1.0

| Проверка | Результат |
| --- | --- |
| ECM / WPA2 / router, esp32s3, вызов build.sh без аргумента защиты | PASS, app 810192 байта |
| RNDIS / WPA2 / router | PASS, app 810144 байта |
| ECM / WPA1 / router | PASS, app 810224 байта |
| ECM / OPEN / router | PASS, app 810064 байта |
| ECM / WPA1 / bridge | PASS, app 600240 байт |
| ECM / WPA2 / bridge | PASS, app 600208 байт |
| ASan+UBSan: пароль, USB TX lifetime, CIDR/ACL, HTTP/URL/rewrite | PASS |
| ASan+UBSan: ECM reset / alt 0/1 / control requests | PASS |
| ELF/config: ровно один выбранный security profile | PASS, все шесть сборок |
| ELF/link map: WPA1-wrapper отсутствует в WPA2/OPEN, присутствует и имеет правильный ABI в WPA1 | PASS, все шесть сборок |
| USB descriptors, память, native ROM recovery config | PASS, все шесть сборок |

Новые каталоги сборки включают security profile:
`build-PROFILE-SECURITY-MODE`. Таким образом обычная сборка не наследует WPA1
из прежнего `build-ecm-router`. Старые образы WPA1 2.0.1 и bridge 1.0.0 сохранены.

## Аппаратные проверки, которые ещё требуются

- [x] Прошивка 2.1.0, проверка хэшей, запуск USB ECM, DHCP WAN, страница шлюза и SNTP.
- [x] DNS relay, HTTPS, HTML/Location rewrite, zond.am и pdaxrom.org → www.pdaxrom.org через USB.
- [x] Корректный сертификат принят; expired / wrong.host / self-signed.badssl.com дают 502.
- [x] Два параллельных HTTPS GET/HEAD и три загрузки по 64 КиБ без сброса.
- [ ] PSP/ARK-5: включён WPA2 Support, успешный обмен ключами AES/CCMP.
- [ ] PSP получает LAN IP, gateway и DNS; открывает страницу шлюза и сайты через proxy.
- [ ] Прямой NAT без proxy с PSP.
- [ ] Переподключение USB и потеря/возвращение upstream без зависания.
- [ ] Смена WAN/DNS и реальное пересечение подсетей DHCP.
- [ ] Windows RNDIS/ICS и Ubuntu ECM/shared.
- [ ] 20 циклов USB reset/suspend/unplug и час работы с PSP.

## Образы и регрессия

Полные WPA2-образы, offset **0x0**:

| Образ | Байты | SHA-256 |
| --- | --- | --- |
| `dist/psp-router-ecm-wpa2.bin` | 875728 | `ef4563d6e9a885aceb32885dbc36f0fced124ad0b58efc155c17e0d40dcc1a67` |
| `dist/psp-router-rndis-wpa2.bin` | 875680 | `13c946a32265a61aebb97b24ed3f46ebbf3dcd0de2839977dfac6309808e664a` |

Проверено совпадение байтов bootloader/partition/app в объединённых образах.
Метаданные — `dist/router-v2.1.0-manifest.json`, контрольные суммы —
`dist/ROUTER_WPA2_SHA256SUMS`. `hardware_verified=false`: полная проверка с PSP
ещё не завершена. `dist/router-v2-manifest.json` обновлён для фактически прошитой
2.1.0; метаданные и образы прежней WPA1 2.0.1 сохранены отдельно.

## Аппаратный live-прогон 2.1.0

Выполнен на ESP с USB-компьютера, журнал `dist/router-live-2.1.0.log`,
результаты `dist/router-live-2.1.0.json`, прошивка `dist/flash-router-v2.1.0.log`.

| Проверка | Результат |
| --- | --- |
| UDP DNS relay example.com | PASS |
| Собственная HTML fixture, переписывание HTTPS-ссылки | PASS |
| zond.am | 200, 47863 байта |
| pdaxrom.org | 301 |
| www.pdaxrom.org | 200, 48285 байт |
| Проверки сертификатов и два параллельных запроса | PASS |
| 64 КиБ, три прогона | 4,295 / 4,164 / 4,146 с; 14,90–15,44 КиБ/с |
| DNS/TCP/TLS на ESP, три прогона | 2814 / 2816 / 2802 мс |

Это проверка HTTPS самой ESP; радио и WPA2-handshake PSP этим прогоном не измеряются.

Проверка после получения WAN IP ESP:

```sh
python tools/test_router_live.py WAN_IP_ESP \
  --site zond.am --site pdaxrom.org --site www.pdaxrom.org \
  --report dist/router-live-2.1.0.json
```

Она проверяет DNS, готовность часов, HTTPS, отклонение неверных сертификатов,
переписывание ссылок/редиректов, два параллельных запроса и три загрузки по 64 КиБ.
Connection включает DNS/TCP/TLS, total — сервер и доставку клиенту. Это не
изолированный криптобенчмарк и не замер радио PSP.

Шлюз остаётся ограниченным GET/HEAD HTTP→HTTPS шлюзом с проверкой CA/имени/сроков,
включая cross-signed CA; адаптация современной HTML/CSS/JS-вёрстки не выполняется.

## Диагностика таймаута WPA2

Собран отдельный `build-ecm-wpa2-router-diag` с явно включённым
`CONFIG_PSP_WIFI_DIAGNOSTICS=y`; обычная сборка оставляет его выключенным.
Проверка ELF требует явный `--diagnostics` и проверяет ABI wrappers. Образ —
`dist/psp-router-ecm-wpa2-diagnostic.bin`, метаданные — `dist/wpa2-diagnostic-manifest.json`.
Диагностический образ прошит: esptool проверил хэши и завершился кодом 0.
USB WAN и JSON endpoint работают. Проверены GET snapshot (200), HEAD команды
переключения (405, версия не меняется) и неверная версия (404). Исходная версия
EAPOL — 2; запись ожидает повторного подключения PSP.

Через USB доступны `/debug/wifi` и `/debug/wifi/eapol/1` либо `/2` на порту 8080.
Доступ проверяется по адресу DHCP gateway USB WAN; клиенты Wi-Fi получают 403.
Переключение меняет только внешнюю версию заголовка EAPOL перед генерацией кадров
и их MIC, перезапускает AP; WPA2/RSN, CCMP и пароль сохраняются.
Изменения самих зашифрованных кадров после вычисления MIC нет.

Кольцевой журнал хранит 64 записи: время, направление, MAC, длину, версию EAPOL,
тип/descriptor/key_info и код возврата Wi-Fi API. Секретные ключи, nonce,
значения MIC и payload не копируются. Статус TX относится к возврату Wi-Fi API,
а не к подтверждению доставки по радио. Версия EAPOL 1/2 — проверяемая гипотеза,
причина таймаута пока не установлена.

Сбор журнала: `python tools/wifi_diag.py WAN_IP --seconds 120 --output dist/wpa2-events.jsonl`.
Тест версии 1: добавить `--eapol 1`; WPA2 Support на PSP остаётся включённым.

### Наблюдение исходного EAPOL v2

Пользователь повторил подключение и подтвердил тот же таймаут. Снимок
`dist/wpa2-eapol2-snapshot.json` показывает повторные TX M1: outer version 2,
RSN descriptor 2, key_info 0x008a, длина 99, key data 0, Wi-Fi API result 0.
От PSP 00:23:4e:17:88:02 приходят EAPOL-Start: outer version 1, packet type 1,
длина 32. Ответа M2, сообщений M3/M4 и события MIC_ERROR нет. Причина
пока не установлена; до проверки пароля обмен ещё не дошёл.

Через USB выполнен переход на EAPOL header v1: повторный INIT подтвердил
версию 1, код AP restart — 0. WPA2/CCMP и пароль не изменялись.
Пользователь повторил подключение: тот же таймаут. Снимок
`dist/wpa2-eapol1-snapshot.json` содержит TX M1 с outer version 1 и descriptor 2,
но от PSP снова только EAPOL-Start, без M2/M3/M4. Само изменение версии заголовка
не исправляет подключение. После сравнения восстановлен исходный EAPOL v2;
это подтверждено `dist/wpa2-eapol-restored.json`.

В изученном ARK-5 вызов WPA2-патча зависит от se_config->wpa2 при загрузке
sceNetApctl_Library; [сам патч](https://github.com/PSP-Arkfive/ARK-5/blob/28eec331e46445b101f769161a3c4c148e9d1c14/Compat/PSP/src/wpa2.c#L82)
отключает EAPOL-Start. Его наличие в журнале требует дополнительной сверки
фактически загруженного режима ARK, но само по себе не доказывает причину отказа.
Запрошены точная версия системного ПО PSP и значение WPA2 Support после
перезапуска. Подключённого USB-накопителя PSP на компьютере нет; конфигурация
консоли напрямую пока недоступна.
