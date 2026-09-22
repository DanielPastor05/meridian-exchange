# Meridian: primera versión ejecutable

El proyecto implementa el núcleo de un exchange simulado en C++20. Puedes enviar
órdenes, ver qué operaciones se ejecutan y cerrar el programa. Al abrir el mismo
journal, recupera las órdenes pendientes y continúa la secuencia.

El README en inglés está preparado para un repositorio de portfolio. Describe
el alcance actual y la asistencia de Codex. Las decisiones que tomes a partir
de aquí y las mediciones que puedas reproducir son la parte que podrás defender
en una entrevista.

## Probarlo en tu Windows

Si utilizas el paquete de ejecutables que acompaña a este proyecto, extrae el ZIP
y abre PowerShell en esa carpeta:

```powershell
.\exchange.exe run --journal demo.journal --input .\session.txt
.\exchange.exe replay demo.journal
.\exchange_tests.exe
.\exchange_bench.exe 200000 5
```

La primera ejecución deja una orden de compra de 2 unidades al precio 10012.
El replay tiene que devolver el mismo libro y `state_hash`. Para empezar otra
sesión desde cero, elige otro nombre de journal.

Para escribir órdenes interactivamente:

```powershell
.\exchange.exe run --journal interactivo.journal
```

Introduce una orden por línea, por ejemplo `NEW 100 SELL 10100 5`, después
`NEW 101 BUY 10100 2`, y finalmente `BOOK`. Cada precio es un entero en ticks.
Puedes cerrar la entrada con Ctrl+Z y Enter en la consola de Windows.

## Compilar el código fuente

Abre una consola de desarrollador de Visual Studio con las herramientas de C++
y CMake. Dentro de la carpeta del proyecto:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
.\build\Release\exchange.exe run --journal nuevo.journal --input examples/session.txt
```

Si CMake no está en PATH, Visual Studio Build Tools suele incluirlo. Desde una
PowerShell normal puedes localizarlo sin fijar la ruta de tu cuenta:

```powershell
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$install = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$cmake = Join-Path $install 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
& $cmake -S . -B build -G 'Visual Studio 17 2022' -A x64
& $cmake --build build --config Release --parallel
$ctest = Join-Path (Split-Path $cmake) 'ctest.exe'
& $ctest --test-dir build -C Release --output-on-failure
```

## Qué revisar primero

1. `src/engine.cpp`: cómo se conserva la prioridad precio-tiempo y cómo se cancela.
2. `tests/tests.cpp`: el motor de referencia y las 80.000 comparaciones.
3. `src/journal.cpp`: qué se persiste antes de responder y qué ocurre al reiniciar.
4. `bench/bench.cpp`: qué incluye el cronómetro y qué queda fuera.
5. `docs/ROADMAP.md`: las siguientes entregas, con criterios verificables.

La versión actual tiene entrada por consola. El gateway TCP y los límites de
riesgo por cuenta son las siguientes etapas. El benchmark mide un núcleo en
memoria con libros pequeños; sus cifras no son latencias de un exchange en red.
