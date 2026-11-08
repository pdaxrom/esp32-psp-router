# RGB status 2.3.0

Расширяет проверенный bridge-proxy 2.2.0; также работает в router и обычном bridge.
Рабочие образы 2.2.0 сохранены отдельно. AP, WPA2/AES, USB и browser proxy не
меняют свои настройки.

## Реализация

- WS2812 на GPIO48: GRB/MSB first, RMT 10 МГц, 24 бита и reset 300 мкс в 25
  символах. Весь кадр помещается в RMT SRAM, программного bit-banging нет.
  Тайминги 0,3/0,9 мкс взяты из официального `peripherals/rmt/led_strip`
  закреплённой ESP-IDF; copy encoder и канал также штатные.
- Базовый цвет: красный без активного USB Ethernet, жёлтый без подтверждения
  интернета, зелёный после HTTP 204. Голубой импульс 200 мс каждые 2 с —
  наличие ассоциированного Wi-Fi-клиента. Светодиод обновляется отдельной задачей.
- Счётчик клиентов обновляется Wi-Fi событиями по MAC, до 4 клиентов; повторные
  события не увеличивают счётчик. MAC конкретной PSP не зашит в прошивку.
- Internet probe: Google connectivitycheck и резервный Cloudflare; отдельная
  задача, DNS до 3 с, общий TCP/send/receive deadline до 3 с на сервер,
  повтор через 15 с. Нет TLS-буферов или использования слотов browser proxy.
- DNS запущен в TCP/IP контексте. Контексты и очереди имеют постоянное время
  жизни, поэтому поздний callback после таймаута не обращается к стеку worker.
- Поколение сети меняется при USB epoch/link, DHCP IP/mask/gateway/DNS;
  старые probe results отбрасываются. USB/Wi-Fi индикация не ждёт probe.
- Яркость 16/255, `CONFIG_PSP_STATUS_RGB` и
  `CONFIG_PSP_STATUS_RGB_BRIGHTNESS`; при выключении нет probe/RMT задач.
  Ошибки выделения RMT/очередей/задач не приводят к abort/reboot роутера.
- Внешних зависимостей не добавлено; используется `esp_driver_rmt` из
  ESP-IDF `fce70533fa1fdeef9f903ce5e87ff8d292be992b`.

## Выполнено на компьютере

- Проверены внешние HTTP endpoints: оба вернули 204.
- ASan/UBSan: фрагментированные/усечённые заголовки, 200 captive portal,
  redirect/503, повреждённый status, HTTP/2/103, NUL, 10 000 повреждённых
  ответов; прежние tests DNS/HTTP proxy/subnet/ECM также проходят.
- ECM/WPA2 bridge-proxy: сборка esp32s3 и проверка ELF/USB/памяти/восстановления.

Дополнительные сборки и ELF-проверки:

| USB | Защита | Режим | RGB | Результат |
| --- | --- | --- | --- | --- |
| ECM | WPA2 | bridge-proxy | on | PASS |
| RNDIS | WPA2 | bridge-proxy | on | PASS |
| ECM | WPA1 | bridge | on | PASS |
| ECM | WPA2 | router | on | PASS |
| RNDIS | WPA2 | router | on | PASS |
| ECM | WPA2 | bridge-proxy | off | PASS |

ELF проверяет наличие RMT/probe только при включённом RGB; сохранены отдельные
проверки WPA1/WPA2, bridge/NAPT, TLS verification и BOOT/RESET.
Рабочие объединённые образы 2.2.0 сверены по прежним SHA-256.

Подготовлен `dist/psp-bridge-proxy-ecm-wpa2-v2.3.0.bin`, manifest
`dist/bridge-proxy-v2.3.0-manifest.json`, `dist/RGB_SHA256SUMS`.
ECM app — 825 440 байт, SHA-256
`c53ce963c48750c23a97e41e1777c279237ec383805d3dc632474345c6e6acc3`.

## Выполнено на плате

- ECM/WPA2 bridge-proxy 2.3.0 прошита: bootloader/partition/app, esptool exit 0,
  все хэши проверены, NVS сохранена. ROM serial `D0:CF:13:30:15:4C`.
- После watchdog reset macOS перечислила USB ECM; DHCP сохранил hostname
  **psp-proxy**, IP **192.168.2.4**.
- Стартовая страница ответила HTTP 200. `Status light: USB active, internet
  reachable, Wi-Fi clients 0` подтверждает запуск RMT-индикации и успешный
  HTTP probe с самой ESP. Фактический цвет требует визуальной проверки.
- Лог прошивки — `dist/flash-bridge-proxy-ecm-wpa2-v2.3.0.log`, снимок —
  `dist/rgb-after-flash.html`.
- Полный живой тест с USB-компьютера прошёл при работающих RGB и probe:
  DNS `psp-proxy`/TTL 0, локальный HTTP, проверка CA/имени/срока TLS,
  переписывание HTML/Location, GET+HEAD параллельно, CONNECT 405,
  отсутствие debug-маршрутов. zond.am — 200 / 47 863 байта,
  pdaxrom.org — 301 / 309 байт.
- Три HTTPS-ответа по 64 КиБ: 4,14–4,94 с, 12,95–15,45 КиБ/с;
  DNS/TCP/TLS connection — 2,80–3,53 с. Замер включает сервер и передачу
  USB-компьютеру; это не отдельный криптографический benchmark.
- Результаты — `dist/live-rgb-v2.3.0-recheck.log` и
  `dist/live-rgb-v2.3.0.json`.

Первый прогон остановился на zond.am с 502 при чтении заголовков upstream;
повторный отдельный GET также дал 502. Затем HEAD, GET с параметром, www и
исходный GET прошли, как и полный повторный прогон, без изменения прошивки.
Причина этих двух сбоев не установлена. Первый лог сохранён в
`dist/live-rgb-v2.3.0.log`.

## Подтверждено пользователем на PSP

- RGB горит зелёным при доступном интернете. После подключения PSP появляются
  короткие голубые импульсы, после WLAN off импульсы исчезают, зелёный остаётся.
  Фактические цвета RGB совпали с ожидаемыми.
- На PSP открылись обе страницы: `http://psp-proxy:8080/?rgb=1` и
  `http://zond.am/` с прежним AES-профилем и proxy `psp-proxy:8080`.

## Осталось проверить

- Отключить только внешний интернет компьютера, сохранив USB/питание/Internet
  Sharing: зелёный → жёлтый, после возврата — зелёный. Проверить proxy на PSP.
  Эта ручная проверка отложена пользователем до коммита; успешной не считается.
- USB reset/reconnect: корректное восстановление без старого зелёного состояния;
  красный без USB-связи проверяется при сохранённом отдельном питании.
- Проверить свежий HTTP/proxy-запрос PSP с индикатором после USB-переподключения.
- Длительная работа и влияние текущего Wi-Fi окружения на радиопередачу.

Отклик HTTP probe подтверждает доступность проверочного HTTP-сервера;
доступность всех сайтов, TLS и конкретной игры из этого не следует.
