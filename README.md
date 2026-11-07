# USB ↔ Wi-Fi bridge для PSP-3000 / ESP32-S3 HW-747

Прошивка на C / ESP-IDF для Super Mini ESP32-S3 с чипом **ESP32-S3FH4R2**.
Нужны плата, кабель USB с передачей данных, компьютер и PSP. Дополнительные
аппаратные модули не используются.

**По уточнённому заданию реализован прозрачный L2 bridge.** USB и Wi-Fi находятся
в одной подсети. IP, маску, шлюз и DNS PSP получает непосредственно от DHCP-сервера
компьютера. NAPT, DHCP-сервер и DNS-прокси на ESP отсутствуют. Компьютер по-прежнему
должен раздавать интернет: его Internet Sharing / ICS выполняет маршрутизацию/NAT
между своей внешней сетью и USB. Режим «Сетевой мост» на компьютере для этой схемы
включать не нужно.

```text
Интернет → Wi-Fi/Ethernet компьютера → общий доступ (DHCP, DNS, NAT)
                                      │
                               USB RNDIS / ECM
                                      │
                             ESP32-S3: L2 bridge
                                      │
                    PSP-NET, канал 6, 802.11b, WPA1/TKIP
                                      │
                                   PSP-3000
```

Например, при Windows ICS компьютер может иметь `192.168.137.1/24`, bridge ESP —
`192.168.137.2`, PSP — `192.168.137.3`. Конкретные адреса назначает компьютер;
эти значения не зашиты в прошивку. DHCP-клиент ESP работает на логическом `br0`
для диагностического адреса. Физические порты USB/AP не имеют собственных IP.
Передача кадров PSP не зависит от получения ESP диагностической аренды.

## Зафиксированные версии и изученные примеры

Использована доступная локально ESP-IDF:

| Зависимость | Версия |
| --- | --- |
| ESP-IDF | `v6.1-dev-4811-gfce70533fa` |
| ESP-IDF commit | `fce70533fa1fdeef9f903ce5e87ff8d292be992b` |
| `espressif/esp_tinyusb` | **`2.0.1~1`**, точная версия в manifest |
| `espressif/tinyusb` | **`0.21.0~4`**, точная версия в manifest |
| Xtensa GCC, проверенная среда | `15.2.0`, tool package `esp-15.2.0_20251204` |
| Component Manager, проверенная среда | `3.0.2` |

Хэши пакетов находятся в `dependencies.lock`; файл следует хранить вместе с
исходниками. CMake проверяет commit IDF. Это зафиксированная **development**
ревизия, а не заявление о совместимости со всеми ESP-IDF 6.x. Для исследования
другой ревизии есть явный `-DPSP_ALLOW_UNTESTED_IDF=ON`; после него нужны повторные
сборки и аппаратные проверки. Автоматическое обновление зависимостей не требуется.

Перед реализацией изучены исходники именно установленной ревизии:

- [ESP-IDF network/bridge](https://github.com/espressif/esp-idf/tree/fce70533fa1fdeef9f903ce5e87ff8d292be992b/examples/network/bridge)
  — LwIP bridge, Wi-Fi AP как порт, отключение DHCP на физических портах и
  ограничения жизненного цикла мостовых портов.
- [ESP-IDF tusb_ncm](https://github.com/espressif/esp-idf/tree/fce70533fa1fdeef9f903ce5e87ff8d292be992b/examples/peripherals/usb/device/tusb_ncm)
  — native USB, установка TinyUSB. Направление и сетевой класс примера отличаются
  от этой прошивки; NCM здесь не используется.
- [ESP-IDF softap_sta](https://github.com/espressif/esp-idf/tree/fce70533fa1fdeef9f903ce5e87ff8d292be992b/examples/wifi/softap_sta)
  — изучен вариант NAPT до согласованного перехода на L2 bridge.
- [TinyUSB net_lwip_webserver](https://github.com/hathach/tinyusb/tree/0.21.0/examples/device/net_lwip_webserver)
  — дескрипторы ECM/RNDIS и сетевые callback.
- [Microsoft OS 2.0 descriptors](https://learn.microsoft.com/en-us/windows-hardware/drivers/usbcon/microsoft-os-2-0-descriptors-specification)
  — BOS и привязка RNDIS к штатному драйверу Windows.

`esp_tinyusb 2.0.1~1` предоставляет сетевую обёртку для NCM. Поэтому ECM/RNDIS
подключён через callback TinyUSB и собственный Ethernet-драйвер `esp_netif`,
с ограниченной очередью TX. Managed-компоненты не патчатся.

## Плата, USB и память

Native USB-C использует внутренний PHY: **GPIO19 = D−, GPIO20 = D+**.
Программа не переназначает эти GPIO и не управляет BOOT/GPIO0, RESET/EN или eFuse.
USB работает в device/full-speed режиме; питание поступает от того же кабеля.
Не подключайте D± к другим устройствам.

Параметры FH4R2: **4 МиБ Flash QIO, 80 МГц; 2 МиБ PSRAM Quad SPI, 40 МГц**.
OPI/Octal PSRAM не используется. См.
[данные Espressif о FH4R2](https://docs.espressif.com/projects/arduino-esp32/en/latest/guides/tools_menu.html#embedded-flash)
и [аппаратное руководство](https://docs.espressif.com/projects/esp-hardware-design-guidelines/en/latest/esp32s3/schematic-checklist.html#flash-and-psram).

Название HW-747 само по себе не подтверждает комплектацию конкретного экземпляра.
Перед прошивкой прочитайте маркировку чипа и в ROM download mode выполните:

```sh
. "$HOME/Work/ESP/esp-idf/export.sh"
python -m esptool --chip esp32s3 --port /dev/cu.usbmodemXXXX flash-id
```

Подставьте порт своей платы (`COMx` в Windows, `/dev/ttyACM0` в Linux).
`flash-id` проверяет физическую Flash; PSRAM проверяется при загрузке прошивки.
При загрузке используются `esp_flash_get_physical_size()` и
`esp_psram_get_size()`, включён стартовый тест PSRAM. Если физические объёмы
отличаются от 4 МиБ Flash / 2 МиБ PSRAM, AP не запускается. Для другой
маркировки нужна отдельная конфигурация и проверка ожидаемых объёмов.
Релиз не содержит USB-консоли и периодического диагностического вывода.
**7 ноября 2026 проверен подключённый экземпляр:** ROM определил ESP32-S3 rev 0.2
со встроенными Flash 4 МиБ и PSRAM 2 МиБ; рабочая прошивка подтвердила
`Flash=4194304 PSRAM=2097152`. Результат относится к проверенной плате; остальные
экземпляры HW-747 проверяйте тем же способом.

Таблица разделов: NVS, PHY и factory app размером 3 МиБ; все разделы внутри 4 МиБ.
OTA, Secure Boot и Flash Encryption не включены. Восстановление через ROM USB
остаётся доступно, если оно не было ранее отключено eFuse самим владельцем.

## Сборка

Из корня проекта, с указанной ESP-IDF:

```sh
. "$HOME/Work/ESP/esp-idf/export.sh"
git -C "$IDF_PATH" rev-parse HEAD
sh tools/build.sh rndis
```

Первая сборка скачивает две зафиксированные зависимости из Espressif Component
Registry. Результат: `build-rndis/psp_usb_router.bin`, bootloader и partition table.
Скрипт также проверяет конфигурацию и USB-дескрипторы из готового ELF.

| Хост и защита | Команда | Каталог |
| --- | --- | --- |
| Windows 10/11, WPA1/TKIP | `sh tools/build.sh rndis` | `build-rndis` |
| macOS/Linux, WPA1/TKIP | `sh tools/build.sh ecm` | `build-ecm` |
| Windows, явно открытая диагностика | `sh tools/build.sh rndis open` | `build-rndis-open` |
| macOS/Linux, явно открытая диагностика | `sh tools/build.sh ecm open` | `build-ecm-open` |

Оба класса выбираются **при сборке**. В одном образе одна USB-сетевая функция
без CDC-ACM консоли; автоматического определения ОС нет. Разные PID позволяют
ОС различать RNDIS (`303a:4011`) и ECM (`303a:4012`). VID/PID используются для
локального прототипа; для выпуска устройства нужна собственная назначенная пара.

Пароль по умолчанию **`psp3000wifi`**. Чтобы задать свой:

```sh
idf.py -B build-ecm -DSDKCONFIG=build-ecm/sdkconfig menuconfig
# PSP USB router → WPA1 passphrase
sh tools/build.sh ecm
```

Для Windows замените `build-ecm` на `build-rndis`. Допустимы 8–63 печатных ASCII
символа. Пустой/неверный пароль останавливает AP; открытая сеть автоматически не
включается. Не переключайте профиль ОС/защиты внутри существующего каталога:
используйте соответствующую команду таблицы. Сборки имеют отдельные `sdkconfig`.

Обычный `idf.py set-target esp32s3 && idf.py build` тоже использует
`sdkconfig.defaults` (RNDIS/WPA1), но для переключения профилей удобнее скрипт.

В Windows PowerShell с активированной средой ESP-IDF можно собирать без `sh`:

```powershell
idf.py -B build-rndis -DIDF_TARGET=esp32s3 -DSDKCONFIG=build-rndis/sdkconfig build
python tools/check_build.py build-rndis --profile rndis
```

## Прошивка и BOOT/RESET

1. Подключите native USB-C кабелем с передачей данных.
2. Удерживайте **BOOT**, нажмите и отпустите **RESET**, затем отпустите BOOT.
   Появится USB Serial/JTAG порт ROM-загрузчика.
3. Прошейте выбранную сборку, например ECM:

   ```sh
   idf.py -B build-ecm -DSDKCONFIG=build-ecm/sdkconfig -p /dev/cu.usbmodemXXXX flash
   ```

4. Отпустив BOOT, нажмите RESET, если приложение не запустилось автоматически.
   ROM-порт исчезнет: в релизе USB используется только как сетевая карта.
   Отсутствие `/dev/cu.usbmodem...` / COM-порта после запуска ожидаемо.

Готовые объединённые WPA1-образы находятся в `dist/psp-bridge-rndis-wpa1.bin` и
`dist/psp-bridge-ecm-wpa1.bin`. Они включают bootloader, partition table и app,
прошиваются **с адреса 0x0** в ROM download mode:

```sh
python -m esptool --chip esp32s3 --port /dev/cu.usbmodemXXXX \
  write-flash 0x0 dist/psp-bridge-ecm-wpa1.bin
```

Объединённый образ также перезаписывает промежутки между разделами, включая
область NVS. Контрольные суммы — `dist/SHA256SUMS`. Для повторного создания образа:

```sh
mkdir -p dist
idf.py -B build-ecm -DSDKCONFIG=build-ecm/sdkconfig \
  merge-bin -o "$PWD/dist/psp-bridge-ecm-wpa1.bin"
```

Отдельный `build-ecm/psp_usb_router.bin` содержит только приложение и размещается
по **0x10000**; команда `idf.py flash` сама берёт правильные адреса из сборки.

Приложение не реализует переход в загрузчик по 1200 baud/DTR. Для следующей
прошивки снова используйте BOOT/RESET: это работает и при ошибке приложения,
неверном пароле или отсутствии PSRAM. Если порт не появился, удерживайте BOOT
при подключении USB, затем отпустите; проверьте кабель и USB-порт. Официальная
[последовательность Espressif](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/get-started/establish-serial-connection.html#flash-using-usb).

## Общий доступ на компьютере

Компьютер должен уже иметь работающий интернет через другой интерфейс. Выбирайте
USB-адаптер ESP как **получателя** общего доступа. Компьютер выдаёт DHCP и PSP,
и диагностическому интерфейсу ESP; статический адрес ESP не нужен.

### Windows 10/11 — RNDIS

1. Прошейте `rndis`. В «Диспетчер устройств → Сетевые адаптеры» должен появиться
   RNDIS/Remote NDIS adapter. COM-порт появляется только в ROM download mode.
2. Прошивка предоставляет RNDIS class/IAD и Microsoft OS 2.0 Compatible ID для
   штатного драйвера. Если автоматическая привязка не сработала, используйте
   «Обновить драйвер → Найти на этом компьютере → Выбрать из списка → Сетевые
   адаптеры → Microsoft → Remote NDIS Compatible Device», если такой драйвер
   доступен в вашей Windows. Сторонний драйвер не нужен. Корпоративная политика
   может запрещать RNDIS/ICS; фактическую работу нужно проверить на вашем хосте.
3. `Win+R` → `ncpa.cpl`. У адаптера, через который работает интернет, откройте
   «Свойства → Доступ». Включите «Разрешить другим пользователям сети использовать
   подключение к Интернету данного компьютера» и выберите **USB RNDIS ESP**.
4. Подтвердите изменения. Проверьте `ipconfig /all`: у USB-адаптера должен быть
   частный IPv4-адрес ICS (часто `192.168.137.1`). Адрес PSP после подключения должен быть
   из той же подсети. Не устанавливайте USB-адаптеру внешний default gateway.
5. Если USB-адаптер был отключён или выбран другой профиль, снова проверьте
   привязку общего доступа. Перезапуск ICS иногда требуется после изменения
   внешнего подключения; прошивка продолжает передавать DHCP без перезагрузки.

Модель службы описана в
[Microsoft SharedAccess](https://learn.microsoft.com/en-us/windows-hardware/customize/desktop/unattend/microsoft-windows-sharedaccess).

### macOS — CDC-ECM

1. Прошейте `ecm`. В «Настройки системы → Сеть» появится USB Ethernet/ECM;
   при необходимости добавьте обнаруженную Ethernet-службу. Штатный драйвер ECM
   не требует HoRNDIS.
2. «Настройки системы → Основные → Общий доступ → Общий интернет» / Internet
   Sharing. Откройте параметры: источник — действующий Wi-Fi/Ethernet,
   получатель («Для компьютеров, использующих») — **USB Ethernet ESP**.
3. Включите общий интернет. Не выбирайте Wi-Fi получателем: точку создаёт ESP.
4. Проверьте `networksetup -listallhardwareports` и `ifconfig`: служба общего
   доступа может использовать `bridge100`, адреса определяет macOS. PSP должна
   получить IP/шлюз/DNS из этой сети.

Порядок меню зависит от версии macOS; см.
[инструкцию Apple](https://support.apple.com/guide/mac-help/share-internet-connection-mac-network-users-mchlp1540/mac).
Обычное назначение USB-адаптеру DHCP-клиента без включения Internet Sharing не
раздаёт адреса PSP. Службы content caching/tethered caching не заменяют этот режим.

### Ubuntu / NetworkManager — CDC-ECM

Найдите интерфейс устройства, затем создайте профиль общего доступа:

```sh
nmcli device status
ip -br link
# Замените enxXXXXXXXXXXXX на интерфейс USB ECM ESP:
sudo nmcli connection add type ethernet ifname enxXXXXXXXXXXXX \
  con-name PSP-USB ipv4.method shared ipv6.method disabled
sudo nmcli connection up PSP-USB
nmcli -f GENERAL,IP4 device show enxXXXXXXXXXXXX
```

Если интерфейс уже занят автоматическим профилем, отключите именно тот профиль
либо измените его `ipv4.method` на `shared`. В GUI это «IPv4 → Shared to other
computers / Общий с другими компьютерами». NetworkManager запускает DHCP/DNS и
маскарадинг, обычно используя `10.42.x.1/24`; см.
[документацию `ipv4.method=shared`](https://networkmanager.dev/docs/api/latest/settings-ipv4.html).
Внешнее подключение компьютера должно сохранять default route.

Для анализа DHCP и прозрачности MAC:

```sh
sudo tcpdump -eni enxXXXXXXXXXXXX 'arp or (udp port 67 or 68) or port 53'
```

В кадрах от PSP должен сохраняться её исходный MAC. DHCP Offer/ACK поступает от
компьютера; в настройках PSP нет отдельной «ESP-подсети». Linux также умеет RNDIS,
но базовый профиль для macOS/Linux здесь ECM.

## Подключение PSP-3000

**ARK-5:** выключите **WPA2 Support** в настройках CFW перед подключением к
этой WPA1/TKIP-сети. На проверенной PSP именно включённый WPA2 вызывал
циклический обмен ключами и таймаут; после отключения подключение работает.

1. Включите физический переключатель WLAN на PSP.
2. Settings → Network Settings → **Infrastructure Mode** → New Connection → Scan.
3. Выберите видимую **PSP-NET**. Параметры AP фиксированы: 2,4 ГГц, **только
   802.11b**, канал **6**, beacon 100 TU, до четырёх клиентов.
4. Защита — **WPA-PSK (TKIP)**, пароль `psp3000wifi` либо ваш из menuconfig.
   Прошивка устанавливает именно `WIFI_AUTH_WPA_PSK` и `WIFI_CIPHER_TYPE_TKIP`,
   проверяет чтением обратно и не заменяет их WPA2/WPA3. PMF не требуется.
   Для WPA1 заголовок EAPOL передаётся с версией 1; descriptor version TKIP
   остаётся 1. Это не отключает проверку MIC и не меняет пароль.
5. Address Settings → Easy либо Custom с IP Automatic, DNS Automatic,
   Proxy Do Not Use. Сохраните и выполните Test Connection.
6. Проверьте, что IP/DNS получены из сети общего доступа компьютера. Откройте
   простой сайт по HTTP, который вы знаете как работающий.

В явной сборке `open` на шаге 4 выбирайте **None**. Возвращение к WPA1 требует
прошивки WPA1-сборки; BOOT и ошибки подключения режим защиты не переключают.
WPA1/TKIP — устаревшая защита, выбранная ради совместимости PSP.
Официальные [настройки PSP](https://manuals.playstation.net/document/en/psp/current/settings/details.html)
перечисляют TKIP и автоматическое получение IP/DNS.

Чтобы отделить Wi-Fi/bridge от внешнего интернета, можно на компьютере запустить
HTTP-сервер **в отдельном каталоге с тестовой страницей** (`python3 -m http.server
8000`) и открыть на PSP `http://<USB-адрес-компьютера>:8000/`. Разрешите входящий
TCP 8000 для теста в локальном firewall. Это проверяет локальный путь, но не DNS
и не выход в интернет. Успешная загрузка по IP и неуспех по имени указывает на
проблему DNS/общего доступа. Современные HTTPS/TLS и недоступные старые сервисы
PSP могут не работать при исправном bridge; браузерная ошибка сама по себе не
доказывает сетевую неисправность.

## Отключения и восстановление

- Потеря внешнего интернета при сохранении USB не перезапускает ESP и не меняет
  защиту AP. После восстановления компьютером общего доступа новые соединения
  снова проходят. Прерванные TCP-сеансы приложениям нужно открыть заново.
- USB reset, unmount и suspend прекращают приём кадров и изменяют номер сеанса
  USB. Управляющий цикл каждые 100 мс меняет link/up состояния USB-порта и br0;
  DHCP-клиент br0 запускается заново при подключении.
- Объекты сетевых портов сохраняются весь срок работы: остановка/пересоздание
  одного порта нарушает ссылки FDB моста в текущем lwIP. При reset не вызывается
  `esp_netif_action_stop()` и не добавляются повторно те же порты.
- TX не блокирует TCP/IP или USB-задачу: очередь максимум 12 копий кадров,
  переполнение приводит к отбрасыванию. Отправка выполняется в задаче TinyUSB
  по SOF; кадры старше 500 мс и кадры другого USB-сеанса отбрасываются.
- ECM-передача дополнительно разрешается только после успешного `SET_INTERFACE`
  с alternate setting 1. Это закрывает сохранение `can_xmit` в TinyUSB при reset
  и отключении data interface; исходники зависимости не изменяются.
- Если новый DHCP-адрес br0 показывает изменение подсети/шлюза компьютера,
  клиенты Wi-Fi деаутентифицируются для обновления аренды. PSP может потребовать
  повторно выбрать сохранённое соединение. При неизменной подсети AP не сбрасывается.
- При физическом отключении единственного кабеля плата теряет питание. Повторное
  подключение — обычная холодная загрузка; сохранение питания без USB не требуется.

## Проверки

**На проверенной PSP с ARK-5 необходимо выключить WPA2 Support в настройках
CFW**, затем подключаться как WPA-PSK (TKIP). Пользователь подтвердил успешное
подключение после отключения WPA2; Mac выдал PSP адрес `192.168.2.5`, исходный
MAC PSP сохранился в ARP на `bridge100`. С включённым WPA2 возникал циклический
обмен ключами и таймаут. Патч ARK [меняет EAPOL на RSN/WPA2](https://github.com/PSP-Archive/ARK-4/blob/main/core/compat/psp/wpa2.c).

Релиз собран без WPA-трассировки, сниффера, самопроверки HMAC, USB-консоли,
периодического вывода и принудительных 1 Мбит/с. Скорость выбирается драйвером
в пределах 802.11b. BOOT/RESET и проверка физической памяти сохранены.
Подробная матрица выполненных проверок — в `TEST_REPORT.md`.

В macOS 27 непривилегированное приложение может получать подставной MAC
`02:00:00:00:00:00`. [Apple подтверждает ограничение](https://developer.apple.com/forums/thread/841958).
На проверенном Mac `sudo ifconfig en13` показывает фактический адрес
`d2:cf:13:30:15:cd`. При Internet Sharing адрес сети раздачи находится на
`bridge100` (`192.168.2.1` в этом прогоне), участник `en13` может иметь link-local
адрес `169.254.x.x`.

```sh
sh tools/test_host.sh
python tools/check_build.py build-rndis --profile rndis
python tools/check_build.py build-ecm --profile ecm
```

`test_host.sh` проверяет границы пароля и недопустимость отправки кадров прошлого
USB-сеанса/истёкшего срока с AddressSanitizer и UBSan. Второй тест с настоящими
заголовками зафиксированного TinyUSB проверяет запрет ECM TX после reset/alt 0 и
разрешение после успешного alt 1; аппаратные функции драйвера в нём заменены stub.
`check_build.py` проверяет
настройки и **байты дескрипторов из связанного ELF**, включая endpoint/interface,
ECM alternate settings и RNDIS MS OS 2.0. Эти проверки не эмулируют драйвер Windows,
радиоканал и PSP. Выполненные проверки и аппаратная матрица — в `TEST_REPORT.md`.

## Структура

- `main/main.c` — проверка памяти, WPA1/AP, создание lwIP bridge и состояния линка.
- `main/usb_net.c` — Ethernet-драйвер esp-netif, USB callback и ограниченная TX-очередь.
- `main/usb_descriptors.c` — RNDIS/ECM, BOS/MS OS 2.0, разные MAC хоста/моста.
- `main/usb_ecm_guard.c` — контроль готовности ECM data interface при повторном USB reset.
- `main/bridge_policy.c` — проверка пароля и срока/принадлежности кадров сеансу.
- `main/wpa_compat.c` — EAPOL v1 для WPA1/TKIP без изменения ключей и MIC.
- `sdkconfig.defaults`, `sdkconfig.ecm`, `sdkconfig.open` — воспроизводимые профили.
- `main/idf_component.yml`, `dependencies.lock` — версии и хэши USB-компонентов.
