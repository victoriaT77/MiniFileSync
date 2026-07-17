# MiniSync

Este repositorio contiene la implementacion de MiniSync, un proyecto de sincronizacion de archivos desarrollado para entornos Linux/WSL. 
El sistema ha sido disenado para monitorizar un directorio, detectar los cambios en los archivos y sincronizarlos 
con un directorio de copia de seguridad mediante varios procesos que cooperan entre sí; Evidenciando asi, la aplicacion practica de conceptos de la meteria de Sistemas Operativos, como la gestion de inodos, la exclusion mutua en regiones de memoria compartida y la comunicacion interproceso (IPC).

## Estructura del Proyecto

El diseno del proyecto segmenta las responsabilidades del sistema en archivos independientes estructurados de la siguiente manera:

* **`minisync-project/`**
  * **`obj/`**: Directorio destinado al almacenamiento de los objetos binarios intermedios (.o) generados durante las etapas de compilacion.
  * **`src/`**
    * **`common.h`**: Definiciones de estructuras de datos globales, constantes comunes y nombres clave de los recursos IPC del Kernel.
    * **`main.c`**: Inicializacion de recursos IPC, algoritmo de planificacion y el bucle infinito de monitoreo del Monitor.
    * **`scanner.c`**: Rutina de lectura recursiva de directorios y analisis de metadatos del sistema de archivos regular.
    * **`worker.c`**: Rutina del agente hijo encargado de la copia fisica y control de exclusion mutua en la seccion critica.
    * **`logger.c`**: Inicializacion del nodo especial FIFO y volcado persistente de registros hacia el almacenamiento fisico.
  * **`Makefile`**: Automatizacion de la compilacion con directivas explicitas para enlazado de bibliotecas del sistema.
  * **`README.md`**

##  Caracteristicas  del Sistema

* **Jerarquia Multiproceso (fork)**: No usamos hilos. El sistema crea procesos independientes que corren en sus propias zonas de memoria virtual aisladas.
* **Sincronizacion Incremental**: El programa no copia todo desde cero. Revisa los metadatos de los archivos y pasa a la cola de copias unicamente lo que ha cambiado.
* **Entrada/Salida Sin Buffer**: Para transferir los datos no usamos printf ni buffers pesados de usuario. Hablamos directo con el Kernel de Linux usando llamadas 
* **Control de Concurrencia (Semaforos)**: Usamos candados logicos (semaforos binarios) para que los Workers no choquen ni alteren mal las estadisticas en la memoria compartida.
* **Aislamiento del Servicio de Registro (FIFO)**: El Logger trabaja en su propio canal independiente para que escribir en el archivo de texto de auditoria no vuelva lento al Monitor.
* **Ciclo de Vida Transparente (Daemon)**: Al pasarle el parametro daemon, el programa rompe lazos con la terminal actual y se queda trabajando de forma invisible en el fondo.


##  Requerimientos vs. Implementacion en Codigo

A continuacion, se detallanN los requerimientos exigidos en la guia deL proyecto y las estrategias tecnicas utilizadas en el codigo para darles cumplimiento :

### A. Scanner Recursivo de Directorios
* **Requerimiento:** Recorrer el directorio recursivamente, recopilar nombre, numero de i-nodo, tamano, permisos y fecha de modificacion de los archivos usando `readdir()`, `closedir()`, `stat()` y `lstat()`.
* **Codigo (`src/scanner.c`):**
  * Se implemento la rutina `recorrer_directorio_recursivo` que abre flujos de directorios iterando descriptores mediante un ciclo `while((entrada = readdir(dir)) != NULL)`.
  * Se discrimina la naturaleza de la entrada mediante la macro `S_ISDIR(info_archivo.st_mode)` para auto-invocar la funcion ante subcarpetas, garantizando recursividad pura sobre el arbol de directorios.
  * Se extraen los metadatos directamente de las estructuras del inodo mediante la llamada al sistema **`lstat()`**, impidiendo que el escanner falle o entre en bucles infinitos al procesar enlaces simbolicos. Se recopilan campos explicitos: `st_ino` (i-nodo), `st_size` (tamano), `st_mode` (permisos) y `st_mtime` (fecha de modificacion), almacenandolos en el vector dinamico `struct FileMetadata`.

### B. Sincronizacion Incremental
* **Requerimiento:** Copiar unicamente los archivos cuya fecha de modificacion o tamano hayan cambiado, optimizando los recursos de Entrada/Salida.
* **Mapeo en Codigo (`src/main.c`):**
  * Se diseno el algoritmo evaluador dentro de la funcion `archivo_requiere_sincronizacion()`.
  * Esta rutina realiza una busqueda indexada en memoria lineal contrastando el estado actual contra el ciclo de muestreo previo. La condicion imperativa de inclusion en la cola de copias es: `if (actual->size != anteriores[i].size || actual->modification_time != anteriores[i].modification_time)`. Si el archivo no ha sido alterado, se descarta su procesamiento, mitigando el overhead en el subsistema de almacenamiento.

### C. Arquitectura Multiproceso y Tuberias (Pipes)
* **Requerimiento:** El monitor comunica las tareas a los workers mediante pipes (`pipe()`, `fork()`, `read()`, `write()`). Mensaje de ejemplo: `COPIAR archivo.txt`. Distribucion equitativa de carga.
* **Mapeo en Codigo (`src/main.c` y `src/worker.c`):**
  * Ante la deteccion de cambios estructurales, el Monitor instancia una matriz de tuberias anonimas mediante `pipe(pipes[i])`. Posterior a la bifurcacion con `fork()`, el Monitor clausura de inmediato los canales de lectura duplicados (`close(pipes[i][0])`) y los Workers clausuran los de escritura (`close(pipes[i][1])`) para asegurar la unidireccionalidad canonica del flujo IPC.
  * La distribucion de carga se procesa bajo el principio de **Planificacion por Turnos (Round-Robin)** mediante una operacion de residuo matematico: `int worker_asignado = i % NUM_WORKERS;`.
  * El Monitor empaqueta la tarea concatenando el comando textual explicito exigido: `snprintf(comando_tarea, ..., "COPIAR %s", ruta)`. El Worker lee síncronamente el buffer crudo mediante `read(pipe_lectura, buffer_msg, MAX_PATH)`, valida los primeros 7 bytes con `strncmp` y extrae la ruta del archivo regular.

### D. Creacion de Copias de Seguridad (E/S Pura)
* **Requerimiento:** Copiar los archivos al directorio `backup/` usando unicamente `open()`, `read()` y `write()`. Prohibido el uso de `cp`, `rsync` o abstracciones de alto nivel (`system()`). Implementar la funcion `copiarArchivo()`.
* **Mapeo en Codigo (`src/worker.c`):**
  * Se implemento formalmente la rutina `copiarArchivo()`. Solicita un descriptor de lectura de bajo nivel mediante `open(origen, O_RDONLY)`.
  * Instancia o sobreescribe el destino en la carpeta de resguardo con los flags del Kernel de Linux: `open(destino, O_WRONLY | O_CREAT | O_TRUNC, 0644)`. El modo `0644` define permisos restrictivos nativos (Lectura/Escritura para el propietario, Lectura para el grupo y el resto del sistema).
  * La transferencia masiva de datos se realiza sin buffers de la biblioteca estandar de usuario (`stdio.h`), iterando bloques binarios crudos en un bucle cerrado mediante `read(fd_origen, buffer, BUFFER_SIZE)` y `write(fd_destino, buffer, leidos)`, garantizando una operacion zero-copy a nivel de aplicacion.

### E. Memoria Compartida y Semaforos (Mutacion Segura)
* **Requerimiento:** Proteger las estadisticas compartidas de condiciones de carrera utilizando `shm_open()`, `mmap()`, `sem_open()`, `sem_wait()` y `sem_post()`.
* **Mapeo en Codigo (`src/main.c` y `src/worker.c`):**
  * El Monitor crea una region de memoria compartida POSIX nominada en el sistema mediante `shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666)` y moldea su dimension en bytes con `ftruncate()`.
  * Los Workers mapean este segmento anonimo en sus mapas de memoria virtual privada a traves de `mmap(..., MAP_SHARED, shm_fd, 0)`.
  * Al tratarse de un recurso compartido de mutacion concurrente, se introdujo un semaforo POSIX nombrado inicializado en `1` (`sem_open(SEM_NAME, O_CREAT, 0666, 1)`). Antes de alterar los contadores globales de la estructura `shared_stats` (archivos copiados, bytes transferidos o errores), los Workers ejecutan **`sem_wait(sem)`** para adquirir de forma exclusiva el token de control, bloqueando a cualquier otro hilo de ejecucion concurrente. Al abandonar la **Seccion Critica**, liberan el recurso asincronamente mediante **`sem_post(sem)`**.

### F. Servicio de Registro (Logger y FIFO)
* **Requerimiento:** Proceso independiente (logger) que recibe mensajes de los workers a traves de una FIFO con nombre (`mkfifo()`). Ejemplo de registro: `[fecha-hora] copiado informe.pdf`.
* **Mapeo en Codigo (`src/logger.c` y `src/worker.c`):**
  * El Logger se independiza del Monitor al arrancar el programa mediante una bifurcacion inicial. Crea un nodo especial en el sistema de archivos utilizando **`mkfifo(FIFO_NAME, 0666)`**. Se bloquea en modo escucha pasiva al abrir el descriptor en lectura pura (`open(FIFO_NAME, O_RDONLY)`).
  * Cuando un Worker concluye una operacion de Entrada/Salida, interrumpe el canal IPC inyectando un string formateado. La construccion de la marca de tiempo decodifica la estructura del sistema `struct tm` calculando de forma exacta el ano (`t->tm_year + 1900`), mes (`t->tm_mon + 1`) y dia (`t->tm_mday`). El Logger despierta ante la llegada de bytes, lee el mensaje del conducto y lo escribe de manera atomica en el archivo de texto utilizando `O_APPEND` para asegurar persistencia acumulativa.

### G. Ciclo de Vida como Demonio (Daemon)
* **Requerimiento:** Convertir el monitor en un demonio mediante la secuencia clasica de desvinculacion de terminal: `fork(); setsid(); chdir("/");`.
* **Mapeo en Codigo (`src/main.c`):**
  * Al ingresar el argumento opcional `daemon`, se invoca la rutina `convertirse_en_demonio()`. Se ejecuta un primer `fork()` para huerfanar el proceso y heredar el control al proceso primigenio `init` (PID 1).
  * Acto seguido, se invoca **`setsid()`** para romper los vinculos con el grupo de procesos anterior, abandonando la sesion y la terminal de control (`tty`). Se implementa la mascara de senales `signal(SIGCHLD, SIG_IGN)` para delegar en el nucleo la recoleccion de codigos de retorno de los hijos y neutralizar la proliferacion de procesos en estado **Zombi**.
  * Finalmente, se realiza una segunda bifurcacion de seguridad para evitar que el proceso re-adquiera descriptores de terminales de caracteres, y se clausuran los descriptores estandar de Entrada/Salida (`STDIN_FILENO`, `STDOUT_FILENO`, `STDERR_FILENO`), completando la metamorfosis a un servicio transparente de segundo plano.



## Compilacion en WSL

### Compilacion Automatizada
El proyecto cuenta con un archivo de configuracion modular (`Makefile`) para optimizar el proceso de construccion de los objetos binarios intermedios y el enlazado final de las librerias del sistema de tiempo real (`-lrt`) y control multi-hilo (`-pthread`).

Para compilar el proyecto completo ejecute:
```bash
make
