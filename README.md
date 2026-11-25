# TeamTalk Extro for Windows

**TeamTalk Extro** — расширенный, доступный (screen-reader friendly) клиент TeamTalk 5 для Windows с профессиональными голосовыми эффектами (24 режима включая Auto-Tune и Studio Voice Rack), продвинутым Караоке-микшером, захватом системного звука (WASAPI Loopback), встроенным Soundboard, проверкой пинга и просмотром пользователей на сервере без входа в канал (Server Peek).

*An enhanced, accessible Windows client for TeamTalk 5 featuring a 24-mode Voice Changer (with real-time Auto-Tune & Studio Rack), Karaoke Pro mixer with WASAPI System Audio Loopback & Auto-Ducking, built-in Soundboard, Server Ping & Server Peek, and cross-platform JSON Backup/Restore compatible with TeamTalk Extro Android.*

---

## ✨ Возможности / Features (v5.31.0)

### 🎭 Voice Changer Pro (24 режима / 24 Modes)
- **Реальный Auto-Tune в реальном времени** (`Auto-Tune Pop`, `Auto-Tune Hard / T-Pain`): детектор основного тона F0 (80–700 Гц), квантизация по ладу (`Chromatic`, `Major`, `Minor`), плавный или жёсткий переход.
- **Studio Voice Rack**: Noise Gate + мягкий RMS-компрессор/лимитер + тёплый бас/присутствие EQ.
- **Broadcast DJ / Radio DJ**: плотная FM-компрессия, тёплый низ, кристальные высокие и мягкий лимитер.
- **Concert Hall**, **Echo / Reverb** (настраиваемые Эхо, Реверберация, Размер комнаты).
- **Голосовые преобразования**: `Deep Voice`, `Chipmunk`, `Female Voice`, `Male Baritone`, `Helium`, `Demon`, `Monster`, `Robot`, `Alien`, `Ghost`, `Chorus`, `Underwater`, `Radio`, `Megaphone`, `Telephone`, `Walkie-Talkie`, `Whisper / ASMR`.
- Поддержка предпрослушивания эффектов при тесте микрофона (`Preferences -> Sound System -> Test Selected`).

### 🎤 Karaoke Pro + Soundboard + WASAPI System Audio Loopback
- **Локальный мониторинг минусовки** в реальном времени в ваши наушники/колонки и передача в канал без необходимости держать PTT/VOX.
- **Захват системного звука / браузера (WASAPI Loopback)**: транслируйте звук из YouTube, браузера, плеера или RadioBOSS прямо в голосовой канал **без VB-Cable** и без локального эха.
- **Auto-Ducking**: автоматическое мягкое приглушение минусовки (~48%) во время речи/пения с плавным возвратом громкости.
- **Key / Pitch Shift (±6 полутонов)**: смена тональности минусовки в реальном времени без изменения темпа.
- **Пауза / Продолжить** (`Ctrl+Shift+P`), **Стоп** (`Ctrl+Shift+S`), настройка громкости и авто-статус (`🎤 Пою: <трек>`).
- **Встроенный Soundboard / Jingle Machine** (`Ctrl+Alt+1..6`):
  - Аплодисменты (`Applause`), Барабанная дробь (`Ba-dum-tss`), Фанфары (`Fanfare`), `DJ Airhorn`, Колокольчик (`Bell / Ding`), Баззер (`Buzzer`) + запуск собственных аудиофайлов-джинглов.

### 🌐 Server Ping Check & Server Peek («Кто на сервере»)
- **Ping Check**: асинхронный замер TCP-пинга (`ms`) для выбранного сервера или всего списка серверов (колонка `Ping`, доступно для программ экранного доступа).
- **Server Peek (`Who's Online (Peek)...`)**: быстрый просмотр дерева каналов и всех пользователей онлайн **без входа в канал**, с кнопками быстрого подключения и обновления.

### 💾 Кроссплатформенный бэкап (JSON) и Автообновление
- **Единый формат бэкапа (`teamtalk-extro-backup` v1)**: экспорт/импорт серверов и настроек TeamTalk Extro в JSON — полностью совместим с **TeamTalk Extro для Android** (переносите списки серверов между ПК и телефоном за пару кликов).
- **GitHub Auto-Update Checker**: проверка новых релизов в `Help -> Check for TeamTalk Extro Updates...`.

---

## 📦 Установка / Installation

Скачайте последний релиз со страницы [Releases](https://github.com/Daket52i/teamtalk-extro-windows/releases):

1. **Установщик (`TeamTalkExtro-*-win64-setup.exe`)** — стандартная установка для Windows 10/11 x64 с поддержкой файлов `.tt` и ссылок `tt://`.
2. **Портативная версия (`TeamTalkExtro-*-win64-portable.zip`)** — распакуйте архив в любую папку и запустите `TeamTalkExtro.exe`.

---

## 🛠️ Сборка из исходников / Building from Source

Проект собирается автоматически через GitHub Actions (`.github/workflows/extro-windows.yml`) с использованием:
- Windows x64, Visual Studio 2022 (MSVC C++17)
- Qt 6.12 (`Widgets`, `Xml`, `Network`, `Multimedia`, `TextToSpeech`)
- CMake, MSYS2, NASM, Inno Setup 6

---

## 🙏 Благодарности / Credits & License

- Основано на [BearWare/TeamTalk5](https://github.com/BearWare/TeamTalk5) (GPLv3 / TeamTalk 5 SDK).
- Голосовые эффекты и идеи кроссплатформенных функций вдохновлены [nekit-wq/Teamtalk-next](https://github.com/nekit-wq/Teamtalk-next).
- Сборка и развитие **TeamTalk Extro Windows**: [Daket52i](https://github.com/Daket52i/teamtalk-extro-windows).
