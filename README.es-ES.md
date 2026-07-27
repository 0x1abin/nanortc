# NanoRTC

English | [简体中文](README.zh-CN.md)

Una implementación de WebRTC en C puro y Sans I/O para RTOS y sistemas embebidos.

> **Implementación nativa de IA**: Cada línea de código en este repositorio —fuente de la biblioteca, pruebas, sistema de construcción, CI, documentación y ejemplos— ha sido escrita por agentes de codificación de IA. Los humanos dirigen la arquitectura y verifican la corrección; los agentes ejecutan. Consulta [Cómo se construye este proyecto](#cómo-se-construye-este-proyecto) para más detalles.

## ¿Qué es NanoRTC?

NanoRTC es una pila de protocolos WebRTC diseñada desde cero para microcontroladores con recursos limitados que ejecutan FreeRTOS, Zephyr, RT-Thread y otras plataformas RTOS.

**Arquitectura Sans I/O** — Inspirada en [str0m](https://github.com/algesten/str0m) (Rust), NanoRTC es una máquina de estados pura. Nunca interactúa directamente con sockets, hilos, asignación de memoria o relojes. Tu aplicación es la dueña del bucle de eventos y de toda la E/S (I/O). Esto hace que NanoRTC sea portable a cualquier plataforma y testeable sin necesidad de una red.

```
                     ┌─────────────────────────┐
  bytes UDP ────────►│                         │──────► bytes para enviar
  tiempo monotónico ──►│  nanortc_t              │──────► eventos de aplicación
  comandos de usuario ──►│  (máquina de estados)   │──────► próximo timeout (ms)
                     │                         │
                     │  Sin sockets. Sin hilos.│
                     │  Sin malloc. Sin relojes.│
                     └─────────────────────────┘
```

## Características

- **Flags de características ortogonales** — Incluye solo lo que necesites:

| Configuración | Flash (.text) | RAM (sizeof) | Flags |
|--------------|---------------|-------------|-------|
| Solo Core | 29.0 KB | 10.2 KB | DC=OFF AUDIO=OFF VIDEO=OFF |
| DataChannel | 38.8 KB | 19.4 KB | DC=ON |
| Solo Audio | 40.8 KB | 20.6 KB | DC=OFF AUDIO=ON |
| DataChannel + Audio | 50.6 KB | 29.9 KB | DC=ON AUDIO=ON |
| Solo Media (sin DC) | 45.3 KB | 51.0 KB | DC=OFF AUDIO=ON VIDEO=ON |
| Media completa | 55.0 KB | 60.3 KB | DC=ON AUDIO=ON VIDEO=ON |

> Medido en ESP32-P4 (RISC-V HP), ESP-IDF 5.5 mbedTLS, `-Os` (`CONFIG_COMPILER_OPTIMIZATION_SIZE=y`). `sizeof(nanortc_t)` es la RAM total por conexión — sin asignación en heap. Las cifras de Flash cuentan solo el código de la biblioteca nanortc (`libnanortc.a` .text); mbedTLS y lwIP son independientes y normalmente se comparten con el resto del firmware.
> Los tamaños reflejan los valores predeterminados de Kconfig de ESP-IDF — tamaño de buffers/colas grado-IoT integrado, pila ICE completa intacta (relé TURN, descubrimiento srflx, candidatos de host IPv6, percepción TWCC/BWE, endurecimiento RFC 8445). Reprodúcelo con `./scripts/measure-sizes.sh --esp32 esp32p4`; ajústalo más a través de `idf.py menuconfig` o [`NANORTC_CONFIG_FILE`](docs/engineering/memory-profiles.md).

Cualquier combinación funciona: audio sin DataChannel, video sin audio, etc.

- **ICE** — Roles controlados (answerer) y controlantes (offerer), trickle ICE, reinicio de ICE.
- **DTLS 1.2** — A través de un proveedor de criptografía enchufable (mbedtls u OpenSSL).
- **SCTP** — Subconjunto mínimo para WebRTC DataChannels (fiable + no fiable).
- **DataChannel** — Protocolo DCEP, modos fiables y no fiables.
- **RTP/RTCP/SRTP** — Transporte de medios de audio y video, paquetización H.264 FU-A y H.265 (RFC 7798).
- **SDP** — Negociación de oferta/respuesta, medios multi-pista.
- **Atravesamiento de NAT** — Descubrimiento server-reflexive de STUN + cliente de relé TURN (opcional, `NANORTC_FEATURE_TURN`).
- **Estimación de ancho de banda** — REMB + BWE de receptor consciente de pérdidas TWCC para video adaptativo.
- **Única dependencia externa** — Solo mbedtls (integrado en ESP-IDF, Zephyr, RT-Thread, STM32).

## Inicio Rápido

```bash
# Construir (Linux/macOS) — predeterminado: Solo DataChannel
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure

# Habilitar audio + video
cmake -B build -DNANORTC_FEATURE_AUDIO=ON -DNANORTC_FEATURE_VIDEO=ON

# Con OpenSSL (para desarrollo en host Linux)
cmake -B build -DNANORTC_CRYPTO=openssl

# Construir ejemplos (media completa)
cmake -B build -DNANORTC_FEATURE_DATACHANNEL=ON -DNANORTC_FEATURE_AUDIO=ON \
      -DNANORTC_FEATURE_VIDEO=ON -DNANORTC_CRYPTO=openssl -DNANORTC_BUILD_EXAMPLES=ON
cmake --build build

# ESP-IDF
idf.py build
```

## Uso

> Las firmas a continuación están simplificadas para mayor legibilidad — se omiten los parámetros de salida opcionales. Consulta [include/nanortc.h](include/nanortc.h) para la API completa.

**Configurar, negociar, ejecutar:**

```c
#include "nanortc.h"

nanortc_t rtc;
nanortc_init(&rtc, &(nanortc_config_t){
    .crypto = nanortc_crypto_mbedtls(),   // o nanortc_crypto_openssl()
    .role   = NANORTC_ROLE_CONTROLLED,    // o _CONTROLLING para ofrecer
});
nanortc_add_local_candidate(&rtc, local_ip, local_port);

char sdp[4096];
nanortc_accept_offer(&rtc, remote_offer, sdp);
// Offerer: create_datachannel("chat") → create_offer(sdp) → accept_answer(remote_answer)
```

El bucle de eventos es simétrico: **vaciar salidas, alimentar entradas.** Tú posees el socket y el reloj:

```c
for (;;) {
    nanortc_output_t out;
    while (nanortc_poll_output(&rtc, &out) == NANORTC_OK)
        handle_output(&out);              // enviar UDP, disparar evento de app, anotar próximo despertar

    size_t len = recv_udp(fd, buf, sizeof buf, &src, wake_ms);
    nanortc_handle_input(&rtc, &(nanortc_input_t){
        .now_ms = now_ms(), .data = buf, .len = len, .src = src,
    });
}
```

Una estructura de entrada entra, una estructura de salida sale. Sin estado oculto, sin hilos en segundo plano.

Para ejemplos completos ejecutables —interoperabilidad con navegador, streaming de cámara en macOS, DataChannel en ESP32— consulta [examples/](examples/).

## Soporte de Plataformas

| Plataforma | Estado | Notas |
|----------|--------|-------|
| Linux / macOS | Desarrollo y pruebas en host | OpenSSL o mbedtls |
| ESP-IDF (ESP32) | Objetivo embebido primario | mbedtls integrado, lwIP |
| Zephyr | Soportado | mbedtls integrado, lwIP |
| RT-Thread | Soportado | paquete mbedtls, lwIP |
| STM32 + FreeRTOS | Soportado | mbedtls distribuido por ST, lwIP |
| NuttX | Soportado | sockets compatibles con POSIX |

## Estructura del Proyecto

```
include/nanortc.h          Cabecera de API pública única
src/                        Módulos de protocolo (Sans I/O, sin dependencias de plataforma)
crypto/                     Proveedores de criptografía enchufables (mbedtls, openssl)
tests/                      Pruebas unitarias + pruebas end-to-end (no requiere red)
tests/interop/              Pruebas de interoperabilidad contra libdatachannel (C++)
examples/                   Plantillas de aplicación
  common/                   Bucle de eventos, señalización, fuente de media reutilizables
  browser_interop/          Arnés de navegador para DataChannel + media
  macos_camera/             Streaming de cámara/mic de macOS → navegador
  esp32_{datachannel,media,camera}/         Objetivos ESP-IDF
  linux_uvc_camera/         Cámara UVC de Linux → navegador (libx264 / NVENC / Rockchip MPP, -e seleccionable)
  tools/                    Utilidades de desarrollo
  sample_data/              Muestras de media (submódulo de git)
docs/                       Documentos de diseño, planes de ejecución, estándares de ingeniería
```

Consulta [ARCHITECTURE.md](ARCHITECTURE.md) para el grafo de dependencias de módulos y el flujo de datos.

## Cómo Se Construye Este Proyecto

NanoRTC es un experimento de **ingeniería de software nativa de IA**, inspirado en [Harness Engineering](https://openai.com/index/harness-engineering/). Todo el código base es generado por agentes de codificación de IA, siguiendo el principio: **los humanos dirigen, los agentes ejecutan**.

Lo que esto significa en la práctica:

- **Arquitectura y diseño** — Decisiones humanas, capturadas en `docs/design-docs/`
- **Todo el código** — Escrito por agentes de IA: fuente de la biblioteca, pruebas, CI, sistema de construcción, documentación.
- **Puertas de calidad** — Ejecutadas mecánicamente vía CI: includes prohibidos, sin malloc, nomenclatura de símbolos, comprobaciones de formato, matriz de construcción de 7 combinaciones de flags de características, AddressSanitizer.
- **Cumplimiento de RFC** — Las implementaciones de protocolos siguen los RFC como estándar autoritativo, no el código de referencia.
- **Verificación continua** — `./scripts/ci-check.sh` ejecuta localmente las mismas comprobaciones que se ejecutan en GitHub Actions. Detecta automáticamente `ccache`, mantiene los directorios de construcción entre ejecuciones para compilación incremental y expone `--fast` para ciclos rápidos pre-push (omite combinaciones de bajo rendimiento y la suite de interoperabilidad de libdatachannel — segundos en lugar de minutos).

La estructura del repositorio está diseñada para la legibilidad de los agentes: [AGENTS.md](AGENTS.md) sirve como punto de entrada, con una revelación progresiva hacia la documentación más profunda. Las restricciones se aplican mediante código, no por convención.

## Documentación

| Documento | Descripción |
|----------|------------|
| [AGENTS.md](AGENTS.md) | Punto de entrada del agente — comandos de construcción, reglas obligatorias |
| [Guía de Construcción](docs/guide-docs/build.md) | Comandos de construcción, flags de características, fuzz, cobertura, ESP-IDF |
| [ARCHITECTURE.md](ARCHITECTURE.md) | Grafo de dependencias de módulos, modelo de capas, flujo de datos |
| [Especificación de Diseño](docs/design-docs/nanortc-design-draft.md) | Referencia de diseño autoritativa completa |
| [Creencias Centrales](docs/design-docs/core-beliefs.md) | Principios de diseño no negociables |
| [Planes de Ejecución](docs/PLANS.md) | Planes de implementación activos y completados |
| [Puntaje de Calidad](docs/QUALITY_SCORE.md) | Calificaciones de calidad por módulo |
| [Perfiles de Memoria](docs/engineering/memory-profiles.md) | Uso de RAM por configuración, guía de ajuste |
| [Estándares de Codificación](docs/engineering/coding-standards.md) | Nomenclatura, estilo, requisitos de pruebas RFC |
| [Guías de C Seguro](docs/engineering/safe-c-guidelines.md) | Funciones prohibidas, reglas de seguridad de buffers |
| [Índice de RFC](docs/references/rfc-index.md) | Referencias a las especificaciones de protocolos |

## Contribuir

NanoRTC está en desarrollo activo. La pila de protocolos central —DataChannel, Audio, Video/H.264/H.265, ICE+STUN+TURN con cumplimiento de RFC 8445, SRTP y percepción TWCC/BWE— está completa en código y verificada en interoperabilidad contra libdatachannel y Chromium. La Fase 8 de optimización continua y la Fase 9 de trabajo de percepción BWE están en curso; consulta [docs/PLANS.md](docs/PLANS.md) para el estado de la fase actual. Los 22 módulos de la biblioteca están en grado A —probados con fuzz, verificados en navegador, verificados en interoperabilidad con libdatachannel y con más del 80% de cobertura.

Las contribuciones son bienvenidas. Por favor, lee [AGENTS.md](AGENTS.md) para obtener instrucciones de construcción y reglas obligatorias antes de enviar cambios.

## Licencia

[MIT](LICENSE)
