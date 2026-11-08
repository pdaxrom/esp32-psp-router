# PSP Router 2.3.0 — ESP32-S3 USB / Wi-Fi

Прошивка на C / ESP-IDF для Super Mini ESP32-S3 HW-747, проверенного чипа
ESP32-S3FH4R2: **4 МиБ Flash, 2 МиБ Quad SPI PSRAM**. Дополнительные аппаратные
модули не нужны. Компьютер раздаёт интернет через native USB-C
(**GPIO19 D−, GPIO20 D+**); ESP маршрутизирует трафик PSP.

```text
Интернет → компьютер с Internet Sharing / ICS
                 │ USB RNDIS (Windows) / CDC-ECM (macOS/Linux)
                 │ WAN ESP: DHCP-клиент, IP/шлюз/DNS от компьютера
            ESP32-S3: IPv4 NAPT
                 │ Wi-Fi LAN: 192.168.50.1/24, свой DHCP и DNS relay
                 │ PSP-NET / 802.11b / канал 6 / WPA2-PSK AES-CCMP
              PSP-3000
```

Вариант 1, прозрачный bridge, сохранён отдельным режимом сборки. Проверенный
релиз 1.0.0 находится в коммите `692b7ec`, его аппаратный отчёт —
[docs/TEST_REPORT_V1.md](docs/TEST_REPORT_V1.md).

Начиная с 2.1.0 по умолчанию включён **WPA2-PSK / AES-CCMP** для ARK-5 с активным
**WPA2 Support**. PMF отключён. Требования сверены с
[исходником ARK-5](https://github.com/PSP-Arkfive/ARK-5/blob/28eec331e46445b101f769161a3c4c148e9d1c14/Compat/PSP/src/wpa2.c).
Для обычной прошивки PSP выбирайте отдельную сборку **wpa1**.

**WPA2/AES подтверждён на PSP-3000 с 6.61 ARK-5 cIPL после полного перезапуска PSP.**
Журнал показал M1 → M2 → M3 → M4 и подключение станции со штатным EAPOL v2.
До перезапуска после изменения WPA2 Support обмен зацикливался до таймаута.
Рабочая WPA1-версия 2.0.1 сохранена в коммите `350a84a`.

**2.1.3 проверена на PSP: локальная страница и zond.am через proxy работают
с WPA2/AES без радиозахвата.** Исправлена задержка TCP-ответов до обновления ARP:
прошивка сохраняет DHCP-привязки IP/MAC только для подключённых WPA2-клиентов
и удаляет их при уходе клиента, смене аренды или LAN-подсети. Перехватчиков WPA,
USB-консоли, команд изменения EAPOL и временных debug-маршрутов нет.
[Подробности и пакетные наблюдения](docs/TEST_REPORT_WPA2_DATA.md).

## Сеть и браузерный прокси

По умолчанию Wi-Fi имеет **192.168.50.1/24**. PSP получает адрес, шлюз и DNS от
ESP. USB получает отдельную аренду компьютера; например, macOS выдаёт WAN ESP
`192.168.2.x/24`, а шлюз/DNS компьютера — `192.168.2.1`. Это две разные подсети.
При пересечении WAN с LAN выбирается другая частная /24: 192.168.51.0,
172.31.50.0 или 10.77.0.0. После смены LAN PSP переподключается для новой аренды.
Если подходящей подсети нет, NAPT не включается.

Без настройки прокси PSP использует обычный NAT: приложения, игры и HTTP идут
напрямую. Для старого браузера есть отдельный **HTTP→HTTPS шлюз на ESP**, порт
**8080**. Он обращается к сайтам по HTTPS, проверяет цепочку, имя и сроки
сертификата с корневыми CA из ESP-IDF. Часы ESP синхронизируются через SNTP;
до получения времени шлюз возвращает 503 с предложением повторить запрос.
Проверка сертификатов не отключается.

Включена штатная поддержка перекрёстно подписанных цепочек CA
(`CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_CROSS_SIGNED_VERIFY=y`). Она нужна, в частности,
для цепочки Google WE1 → GTS Root R4, которую могут передавать сайты за Cloudflare.
Без неё IDF 6.x может отвергать корректную цепочку, даже если её корень есть в
bundle. [Описание Espressif](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/protocols/esp_crt_bundle.html#cross-signed-certificate-support).

PSP получает страницы по HTTP. Радиоканал PSP↔ESP по умолчанию шифруется WPA2/AES-CCMP;
HTTPS между ESP и сайтом завершается на ESP, сквозного TLS до PSP здесь нет.
Просроченные корни PSP при таком обращении не используются.

Шлюз предназначен для **простых публичных страниц**:

- GET и HEAD; upstream всегда HTTPS, включая запросы `http://` от PSP.
- HTTPS-редиректы и буквальные `https://` в HTML/CSS/JavaScript заменяются на HTTP,
  чтобы следующие обращения браузера тоже проходили через шлюз.
- Ответы передаются потоком как HTTP/1.0 с закрытием соединения; chunked upstream
  декодирует HTTP-клиент ESP. Запрашивается `Accept-Encoding: identity`.
- Два одновременных запроса; заголовки запроса до 4 КиБ, URL до 2 КиБ, ответ до 4 МиБ,
  цикл передачи до 60 с, отдельные сетевые операции ограничены таймаутами.
  Крупные TLS-буферы могут размещаться в PSRAM.
- POST, CONNECT, cookies/авторизация, преобразование изображений, современный JS,
  CAPTCHA и видеосервисы не поддерживаются. Бинарные ответы не переписываются.
- Ошибка сертификата/HTTPS даёт 502; автоматического обхода проверки или отката
  на незашифрованный upstream нет. HTTP-only сайты открывайте без прокси.

На PSP с WPA1/2.0.1 подтверждено открытие ya.ru, zond.am и pdaxrom.org. Современная вёрстка
последних двух отображается некорректно: шлюз передаёт HTML/CSS, преобразование
оформления для старого браузера не выполняется.

CONNECT лишь передаёт TLS между PSP и сайтом и не исправляет старые сертификаты
или несовместимые версии TLS, поэтому он не используется. В браузере вводите
**http://**, например `http://example.com/`, даже если сайт работает по HTTPS.

На `http://192.168.50.1:8080/` находится простая стартовая страница, поле адреса
и последние замеры. Connection включает DNS/TCP/TLS, Total также включает
передачу клиенту; это не отдельный бенчмарк криптографии.
Страница также показывает подключённые Wi-Fi клиенты,
выданные DHCP-адреса и число LAN TCP/HTTP запросов. Адрес в ARP-таблице может
быть создан DHCP-сервером и сам по себе не подтверждает доступность клиента.

На проверенной ESP32-S3 три HTTPS-загрузки по 64 КиБ через USB заняли
4,11–4,21 с каждая, включая 2,79–2,80 с установки соединения: около 15,2–15,6 КиБ/с
за весь запрос. Это измерение с USB-компьютера; скорость браузера PSP и Wi-Fi
пока не измерена. Условия и выполненные проверки — в [TEST_REPORT.md](TEST_REPORT.md).

DNS relay и прокси принимают запросы только от Wi-Fi-подсети и USB-компьютера
(его адрес определяется как DHCP gateway). Остальные источники отклоняются.
На компьютере их можно проверить через WAN-адрес ESP без подключения к PSP-NET.

## Плата и восстановление

### RGB-индикация состояния (2.3.0)

В режимах router, bridge и bridge-proxy включён встроенный **WS2812 RGB на
GPIO48**. Основной цвет и короткий голубой импульс показывают разные состояния:

| RGB | Значение |
| --- | --- |
| Красный | USB-сетевой интерфейс не активен: компьютер не настроил USB или связь сброшена/приостановлена. |
| Жёлтый | USB-сеть активна, но интернет ещё не подтверждён: ожидание DHCP/DNS, первая проверка или её отказ. |
| Зелёный | Получен ожидаемый ответ внешнего HTTP-сервера проверки связности. |
| Короткий голубой импульс каждые 2 с | К Wi-Fi подключён клиент (в нашем использовании — PSP); между импульсами остаётся основной цвет. |

Индикатор клиента означает ассоциацию с AP. Он не подтверждает, что браузер уже
загрузил страницу. Другие Wi-Fi-клиенты также учитываются, MAC PSP не зашит в код.
Красный цвет при отключённом USB виден только если плата продолжает получать
питание; при снятии единственного USB-питания светодиоды гаснут.

Проверка интернета выполняется отдельной задачей: DNS и HTTP GET
`/generate_204` на `connectivitycheck.gstatic.com`, при отказе — `cp.cloudflare.com`.
Успехом считается только HTTP 204 с полными заголовками; страницы авторизации,
редиректы и ответы 200 не принимаются за успешную проверку. Это лёгкая проверка
доступности внешнего HTTP, TLS-проверки браузерного proxy она не заменяет.
Если оба проверочных сервера недоступны, цвет остаётся жёлтым.

Повторная проверка — через 15 с после завершения предыдущей. На сервер отведено
до 3 с на DNS и 3 с на TCP/HTTP; потеря/возврат интернета обычно отражается в
пределах 30 с. Изменение USB/DHCP сбрасывает предыдущий результат немедленно
и запускает новую проверку. Результат старой сетевой сессии не применяется.
RGB и события Wi-Fi обновляются независимо от сетевого ожидания.

Передача RGB выполняется RMT, один кадр целиком помещается в его памяти.
Яркость по умолчанию — **16/255**. Настройки: `menuconfig` → **PSP USB router** →
**Onboard GPIO48 RGB status light / RGB brightness**. Отключение RGB убирает
также фоновые проверки интернета. Ошибка индикатора не перезагружает роутер.
На стартовой странице proxy есть строка **Status light** с текущим состоянием.

Синий индикатор зарядки TP4054 работает независимо от этой индикации. По
[схеме SuperMini](https://wiki.nologo.tech/assets/img/esp32/esp32s3supermini/1.png)
обычный красный LED и вход RGB разделяют GPIO48, поэтому свечение обычного
красного LED при включении RGB может измениться.
Зависимости остаются прежними: используется RMT из закреплённой ESP-IDF,
новых managed-компонентов нет.
Образы с RGB имеют отдельную версию **2.3.0**:
`dist/psp-bridge-proxy-ecm-wpa2-v2.3.0.bin` и
`dist/psp-bridge-proxy-rndis-wpa2-v2.3.0.bin` (offset 0x0).
Состав и контрольные суммы — `dist/bridge-proxy-v2.3.0-manifest.json`,
`dist/RGB_SHA256SUMS`; [проверки RGB](docs/TEST_REPORT_RGB.md).

### Память и загрузчик

Flash: **4 МиБ QIO / 80 МГц**. PSRAM: **2 МиБ Quad SPI / 40 МГц**, не OPI.
CPU в режимах router и bridge-proxy: **240 МГц**, в обычном bridge — 160 МГц.
Аппаратные AES/SHA/RSA включены в IDF.
ROM проверенной платы определил Flash XMC 4MB и PSRAM AP_3v3 2MB; приложение
измерило 4194304/2097152 байт. Перед запуском AP эти физические размеры проверяются;
при несовпадении AP не запускается. Название HW-747 само по себе комплектацию
не гарантирует. Для другого чипа нужна отдельная конфигурация.

Приложение не управляет GPIO0/BOOT, EN/RESET или eFuse. Secure Boot и Flash
Encryption не включены. Native ROM download остаётся доступен через BOOT/RESET.
В обычной прошивке USB используется только как сеть: COM/tty-консоли нет.

## Зафиксированные зависимости и примеры

| Зависимость | Проверенная версия |
| --- | --- |
| ESP-IDF | v6.1-dev-4811-gfce70533fa |
| IDF commit | fce70533fa1fdeef9f903ce5e87ff8d292be992b |
| espressif/esp_tinyusb | 2.0.1~1 |
| espressif/tinyusb | 0.21.0~4 |
| Xtensa GCC | 15.2.0, esp-15.2.0_20251204 |
| Python (сборка 2.0.1) | 3.15.2 |
| Component Manager | 3.1.2 |
| esp-idf-kconfig | 3.14.0 |
| esptool | 5.6.0 |

Manifest и registry-хэши — в `main/idf_component.yml` и `dependencies.lock`.
HTTP-клиент, TLS, CA bundle, SNTP и NAPT входят в закреплённую ESP-IDF; новых
внешних библиотек нет. CMake проверяет commit IDF. `PSP_ALLOW_UNTESTED_IDF=ON`
разрешает другую ревизию только для исследования с повторной проверкой.

Использованы официальные исходники именно этой ревизии:

- [softap_sta / NAPT, DNS DHCP](https://github.com/espressif/esp-idf/tree/fce70533fa1fdeef9f903ce5e87ff8d292be992b/examples/wifi/softap_sta)
- [esp_http_client](https://github.com/espressif/esp-idf/tree/fce70533fa1fdeef9f903ce5e87ff8d292be992b/examples/protocols/esp_http_client)
- [network/bridge](https://github.com/espressif/esp-idf/tree/fce70533fa1fdeef9f903ce5e87ff8d292be992b/examples/network/bridge)
- [tusb_ncm / установка native TinyUSB](https://github.com/espressif/esp-idf/tree/fce70533fa1fdeef9f903ce5e87ff8d292be992b/examples/peripherals/usb/device/tusb_ncm)
- [TinyUSB net_lwip_webserver / ECM и RNDIS](https://github.com/hathach/tinyusb/tree/0.21.0/examples/device/net_lwip_webserver)
- [Microsoft OS 2.0 descriptors](https://learn.microsoft.com/en-us/windows-hardware/drivers/usbcon/microsoft-os-2-0-descriptors-specification)

Managed-компоненты и установленная ESP-IDF не патчатся. В WPA2 используется
штатный RSN/EAPOL v2 ESP-IDF. EAPOL v1 linker-wrapper включается только в
явно выбранной WPA1/TKIP-сборке с тем же private ABI, что и wpa_supplicant.

## Сборка

```sh
. "$HOME/Work/ESP/esp-idf/export.sh"
sh tools/build.sh ecm                 # macOS/Linux, WPA2/AES, router + proxy
sh tools/build.sh rndis               # Windows, WPA2/AES, router + proxy
sh tools/build.sh ecm wpa1 router      # WPA1/TKIP для PSP без WPA2-патча
sh tools/build.sh ecm wpa1 bridge      # прозрачный bridge, WPA1/TKIP
sh tools/build.sh ecm wpa2 bridge      # прозрачный bridge, WPA2/AES для ARK-5
sh tools/build.sh ecm wpa2 bridge-proxy # bridge + HTTPS-шлюз, имя psp-proxy
sh tools/build.sh rndis wpa2 bridge-proxy # то же для Windows
sh tools/build.sh ecm open router     # явно открытая диагностическая сеть
```

Аргументы: `[rndis|ecm] [wpa2|wpa1|open] [router|bridge|bridge-proxy]`.
Каталоги отдельные: `build-ecm-wpa2-router`, `build-rndis-wpa2-router`,
`build-ecm-wpa1-router`, `build-ecm-wpa1-bridge`, `build-ecm-wpa2-bridge`,
`build-ecm-open-router`, `build-ecm-wpa2-bridge-proxy`. Профили ОС выбираются при сборке;
автоматического переключения защиты нет. VID/PID прототипа: RNDIS 303a:4011,
ECM 303a:4012. Перед распространением устройства нужна назначенная VID/PID-пара.

### Bridge: выбор WPA1 или WPA2

В прозрачном мосте защита выбирается вторым аргументом, архитектура — третьим:

```sh
sh tools/build.sh ecm wpa1 bridge      # macOS/Linux, WPA-PSK / TKIP
sh tools/build.sh ecm wpa2 bridge      # macOS/Linux, WPA2-PSK / AES-CCMP
sh tools/build.sh rndis wpa1 bridge    # Windows, WPA-PSK / TKIP
sh tools/build.sh rndis wpa2 bridge    # Windows, WPA2-PSK / AES-CCMP
```

Сборки находятся в отдельных каталогах `build-<usb>-<security>-bridge`.
WPA1 использует EAPOL v1, WPA2 — штатный EAPOL v2. SSID **PSP-NET** и пароль
**psp3000wifi** одинаковы; автоматического переключения защиты нет.
В `menuconfig` этот же выбор доступен в **PSP USB router → Wi-Fi security**.

Для WPA2 на PSP нужны включённый **WPA2 Support** в ARK-5 и профиль **AES**;
для WPA1 — выключенный WPA2 Support и профиль **TKIP**. После изменения
WPA2 Support полностью перезапустите PSP. IP/DNS оставьте автоматическими,
proxy — **Do Not Use**: в bridge адреса выдаёт компьютер в одной подсети,
а HTTP→HTTPS шлюз доступен в router и в отдельном bridge-proxy.

Готовые bridge-образы имеют отдельные имена
`dist/psp-bridge-<usb>-<security>-v2.1.3.bin` и записываются по **0x0**.
Контрольные суммы — `dist/BRIDGE_WPA_SHA256SUMS`, состав вариантов —
`dist/bridge-v2.1.3-manifest.json`.
ECM/WPA2 bridge проверен на PSP: DHCP компьютера, локальный и внешний HTTP,
а также свежий HTTP-запрос после переподключения USB. RNDIS bridge пока проверен
только сборкой. Проверки router и bridge описаны раздельно.
[Отдельная аппаратная проверка bridge/WPA2](docs/TEST_REPORT_BRIDGE_WPA2.md).

### Bridge + proxy с именем psp-proxy

Этот отдельный режим, добавленный в **2.2.0**, объединяет прозрачный мост с тем же
HTTP→HTTPS шлюзом, что и router. Обычный bridge остаётся без прокси.

```sh
sh tools/build.sh ecm wpa2 bridge-proxy
# Каталог: build-ecm-wpa2-bridge-proxy
# Windows: sh tools/build.sh rndis wpa2 bridge-proxy
# WPA1/TKIP: sh tools/build.sh ecm wpa1 bridge-proxy
```

Компьютер по-прежнему выдаёт **и ESP, и PSP адреса в одной подсети**, например
192.168.2.4 и 192.168.2.5. Шлюз и основной DNS PSP — компьютер, например
192.168.2.1. ESP не запускает DHCP-сервер и не включает NAPT.

На PSP оставьте IP/DNS **автоматическими**, а в настройках прокси укажите:

- Адрес: **psp-proxy** (без `http://` и без `.local`).
- Порт: **8080**.

Стартовая страница: **http://psp-proxy:8080/**. Для проверки свежего запроса
можно добавить `?check=1`, затем `?check=2`. Сайты открывайте с `http://`,
например **http://zond.am/**; HTTPS до сайта и проверку сертификата выполняет ESP.
Для прямого доступа игр и HTTP отключите proxy в профиле PSP.

ESP отправляет DHCP hostname `psp-proxy`. Одного hostname недостаточно:
проверенный Internet Sharing macOS не регистрирует его в обычном DNS.
Поэтому в этой сборке ESP отвечает на точный DNS-запрос **psp-proxy** от
подключённой Wi-Fi станции своим текущим DHCP-адресом. Ответ имеет TTL 0;
при смене аренды новые запросы получают новый адрес. Остальные имена и
DHCP-пакеты проходят через мост к компьютеру без изменения.

Используется обычный **IPv4 UDP DNS, порт 53**; это не mDNS/Bonjour и не
системная регистрация имени на компьютере. `psp-proxy.local`, DNS over TCP,
DNSSEC и зашифрованные DNS-запросы не поддерживаются этим локальным ответчиком.
Запросы компьютера к собственному DNS могут вообще не проходить через ESP,
поэтому имя проверяется с PSP. На компьютере можно проверить его напрямую,
подставив текущий адрес ESP из списка DHCP-аренд:

```sh
dig @192.168.2.4 psp-proxy A
python tools/test_router_live.py 192.168.2.4 --mode bridge-proxy --site zond.am
```

Привязка IP/MAC для ответов самой ESP берётся только от подключённых Wi-Fi
станций: из DNS-запроса или ARP. Она удаляется при отключении станции,
USB или смене адреса/подсети ESP. Это сохраняет обход проблемы ARP старого
клиента PSP без собственного DHCP-сервера. Прозрачный транзит не требует этой привязки.
USB-консоли, трассировки WPA и команд изменения EAPOL в образе нет.

Собранные WPA2-образы: `dist/psp-bridge-proxy-ecm-wpa2-v2.2.0.bin` и
`dist/psp-bridge-proxy-rndis-wpa2-v2.2.0.bin` (объединённые, offset **0x0**).
Для обновления с сохранением NVS используйте `idf.py flash` выбранного
build-каталога: он записывает bootloader, partition table и app отдельно.
Хэши и состав — `dist/BRIDGE_PROXY_SHA256SUMS` и
`dist/bridge-proxy-v2.2.0-manifest.json`.

Ограничения HTML/CSS, GET/HEAD и TLS те же, что у router. Рабочие образы
router и bridge **2.1.3** сохранены отдельно; аппаратная проверка нового режима
перечислена в [docs/TEST_REPORT_BRIDGE_PROXY.md](docs/TEST_REPORT_BRIDGE_PROXY.md).
**ECM/WPA2 bridge-proxy 2.2.0 проверена на PSP:** стартовая страница по имени
`psp-proxy` и zond.am через proxy открываются, в том числе по свежим URL после
отключения и подключения USB. ESP подтвердила повторные Wi-Fi DNS-ответы
и IP PSP в общей подсети компьютера.

### Пароль и другие настройки

Пароль по умолчанию **psp3000wifi**. Изменение:

```sh
idf.py -B build-ecm-wpa2-router -DSDKCONFIG=build-ecm-wpa2-router/sdkconfig menuconfig
# PSP USB router → Wi-Fi passphrase
sh tools/build.sh ecm
```

Допустимы 8–63 печатных ASCII-символа. Неверный пароль останавливает AP.
Другие параметры сети/прокси — в том же menuconfig.
Профили ОС и архитектуры меняйте аргументами скрипта, чтобы сохранять отдельные
конфигурации. В Windows PowerShell с активированной средой ESP-IDF:

```powershell
idf.py -B build-rndis-wpa2-router -DIDF_TARGET=esp32s3 -DSDKCONFIG=build-rndis-wpa2-router/sdkconfig build
python tools/check_build.py build-rndis-wpa2-router --profile rndis --mode router
```

## Прошивка

1. Подключите native USB-C кабелем с передачей данных.
2. Зажмите BOOT, нажмите и отпустите RESET, затем отпустите BOOT.
3. Прошейте нужную сборку (подставьте ROM-порт, COMx в Windows):

   ```sh
   idf.py -B build-ecm-wpa2-router -DSDKCONFIG=build-ecm-wpa2-router/sdkconfig \
     -p /dev/cu.usbmodemXXXX flash
   ```

4. Если приложение не запустилось автоматически, нажмите RESET без BOOT.
   ROM-порт исчезнет и появится USB-сеть; отсутствие последовательного порта ожидаемо.

Полный образ `dist/psp-router-ecm-wpa2.bin` записывается по **0x0**.
Один `build-ecm-wpa2-router/psp_usb_router.bin` — только приложение по **0x10000**.
Полный объединённый образ перезаписывает и промежутки, включая NVS.
Для обновления без стирания NVS используйте `idf.py flash`.
Сохранённый рабочий bridge 1.0.0: `dist/psp-bridge-ecm-v1.0.0.bin`.

## Общий доступ компьютера

Компьютер должен иметь отдельное действующее подключение к интернету. USB-карта
ESP — получатель общего доступа. В роутерном режиме DHCP компьютера выдаёт адрес
только WAN ESP; адреса PSP выдаёт уже сам ESP.

### Windows 10/11 — RNDIS

В диспетчере устройств должен появиться сетевой адаптер RNDIS/Remote NDIS.
Используется штатный драйвер с Microsoft OS 2.0 Compatible ID. При необходимости:
«Обновить драйвер → Выбрать из списка → Сетевые адаптеры → Microsoft → Remote NDIS
Compatible Device». Сторонний драйвер не требуется, но привязка требует проверки
на конкретной Windows.

`Win+R` → `ncpa.cpl` → свойства действующего интернет-адаптера → «Доступ» →
разрешить общий доступ и выбрать USB RNDIS ESP. Обычно USB-сеть ICS —
192.168.137.0/24. Не назначайте USB-адаптеру внешний default gateway.

### macOS — CDC-ECM

«Настройки системы → Сеть»: PSP Router ECM/USB Ethernet, штатный драйвер.
«Основные → Общий доступ → Общий интернет»: источник — действующий интернет,
получатель — **USB Ethernet ESP**. Wi-Fi получателем не выбирайте, AP создаёт ESP.

В этой системе используется `bridge100` с адресом 192.168.2.1, участник en13.
Link-local 169.254.x.x на en13 сам по себе не является ошибкой.
В macOS 27 обычный ifconfig может скрывать MAC как 02:00:00:00:00:00;
`sudo ifconfig en13` показывает реальный адрес. [Подтверждение Apple](https://developer.apple.com/forums/thread/841958).

### Ubuntu / NetworkManager — ECM

Выберите обнаруженный USB Ethernet, создайте отдельное подключение с
`ipv4.method shared`, сохранив внешний интернет-интерфейс:

```sh
nmcli device status
sudo nmcli connection add type ethernet ifname enxXXXXXXXXXXXX con-name psp-usb \
  ipv4.method shared ipv6.method disabled
sudo nmcli connection up psp-usb
```

Подставьте свой USB-интерфейс. NetworkManager выдаст WAN ESP адрес своей подсети.

## Подключение PSP

По умолчанию используется **WPA2-PSK / AES-CCMP** для ARK-5.
Включите **WPA2 Support** в настройках CFW и полностью выключите PSP,
удерживая ползунок питания вверх 4–5 секунд, затем включите консоль.
Это обеспечивает повторную загрузку сетевого модуля с патчем.
Создайте новое подключение к **PSP-NET**, чтобы
прежний профиль TKIP не сохранил старый режим защиты.

1. Включите WLAN, Infrastructure Mode → Scan → **PSP-NET**.
2. Выберите защиту с **AES** (в интерфейсе PSP она может называться
   **WPA-PSK (AES)**), пароль **psp3000wifi**.
3. IP и DNS — Automatic, для прямого NAT-доступа Proxy — Do Not Use.
4. В режиме router PSP должна получить **собственный IP 192.168.50.2–192.168.50.101**,
   маску 255.255.255.0, шлюз/DNS 192.168.50.1. Адрес **192.168.50.1 принадлежит ESP**;
   назначение его самой PSP создаёт конфликт и таймаут даже при открытии локальной страницы.
5. Для браузерного шлюза отредактируйте это подключение: Proxy → Use,
   Address **192.168.50.1**, Port **8080**. При смене LAN используйте новый шлюз PSP.
6. Откройте **http://192.168.50.1:8080/**, затем **http://httpbingo.org/html**.
   Не вводите https:// в адресной строке: такой запрос потребует старого TLS PSP.

Можно сохранить два соединения с одним SSID: одно без прокси, второе с прокси.

Для явной сборки `wpa1` используйте **WPA-PSK (TKIP)** и **выключите WPA2 Support**
в ARK-5, затем перезапустите PSP. Этот режим сохранён для обычной прошивки Sony
и прежних подключений; автоматического перехода между WPA2/WPA1/OPEN нет.

## Проверки и отключения

```sh
sh tools/test_host.sh
python tools/check_build.py build-ecm-wpa2-router --profile ecm --mode router
python tools/check_build.py build-ecm-wpa1-bridge --profile ecm --security wpa1 --mode bridge
python tools/test_router_live.py WAN_IP_ESP --report dist/router-live.json
```

Проверяются границы пароля/USB-кадров, пересечения CIDR, HTTP/URL-парсер,
отклонение неоднозначных запросов, переписывание ссылок на всех границах буфера,
готовность ECM после reset/alt 0/alt 1. Проверка ELF читает реальные USB-дескрипторы,
выбранный сетевой режим, CA bundle и отсутствие отладочных функций.

USB reset/suspend меняет номер сеанса, отбрасывает старые кадры и останавливает
WAN DHCP. AP/LAN остаётся доступен. При изменении WAN NAPT-таблица очищается,
активные запросы шлюза завершаются с ограниченным таймаутом; новые идут через
актуальный адрес/DNS. Старые TCP-сеансы могут закрыться. При физическом извлечении
единственного USB-кабеля плата теряет питание.

DNS relay поддерживает UDP-пакеты до 1232 байт; DNS-over-TCP не реализован.
Выполненные аппаратные проверки, замеры и оставшиеся тесты — в [TEST_REPORT.md](TEST_REPORT.md).
