# Meridian Exchange: empieza aquí

El proyecto está en C:\Users\Dani\Documents\meridian-exchange. Está pensado para demostrar ingeniería de sistemas en entrevistas de prácticas: estructuras de datos, redes, contabilidad entera, persistencia, pruebas y medición.

## Primera ejecución

Instala un compilador C++20, CMake y Node.js 24. En Windows abre una terminal de Visual Studio, entra en la carpeta del proyecto y ejecuta:

```powershell
cmake -S . -B build
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
node tools/demo.mjs ./build/Release/exchange_server.exe
```

La demo verifica un cruce parcial entre dos clientes, los saldos, el reinicio con journal, el reintento sin duplicar la operación y la cancelación de las órdenes de una cuenta mediante kill switch.

## Qué debes poder explicar

1. Cómo conservan la prioridad FIFO los índices de la lista aunque se reciclen posiciones de memoria.
2. Por qué el ID de una orden y la secuencia de una petición resuelven problemas distintos.
3. Qué sucede si el proceso cae después del sync pero antes de responder.
4. Cómo se reservan efectivo e inventario, y qué se libera en una ejecución parcial.
5. Por qué cientos de nanosegundos del núcleo no implican confirmaciones persistentes igual de rápidas.
6. Cómo interpretar los descartes y la latencia del generador de carga abierta.
7. Qué recupera un snapshot del feed y qué historial se pierde cuando vence su retención.

Para una presentación de cinco minutos: ejecuta la demo, muestra una prueba de caída, compara el profiler del núcleo con el benchmark síncrono, y explica una limitación con los datos delante. El código fue generado con ayuda de Codex; estudia y modifica una parte antes de atribuirte dominio técnico. Ningún proyecto permite certificar un percentil de candidatos o garantizar una oferta.

La documentación técnica y los datos están en docs/ y bench/results/. El README incluye instrucciones para Linux y para usar clientes manuales. Las claves de ejemplo son públicas y solo sirven para la demo local.
