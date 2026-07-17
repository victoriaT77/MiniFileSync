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
## A. Scanner Recursivo de Directorios
* **Requerimiento:** Recorrer carpetas de forma recursiva y sacar nombre, inodo, tamano, permisos y fecha usando `readdir()`, `closedir()`, `stat()` y `lstat()`.
* **Mapeo en Codigo (`src/scanner.c`):**
  * *Implementacion:* En el codigo, `readdir()` lee cada archivo y la macro `S_ISDIR` detecta si es una subcarpeta para volver a llamar a la funcion. Usamos `lstat()` para leer la cedula de identidad de cada archivo (su inodo `st_ino`, tamano `st_size` y fecha `st_mtime`), garantizando que si hay un acceso directo logico no nos quedemos atrapados en un bucle.

### B. Sincronizacion
* **Requerimiento:** Copiar solo los archivos cambiados en tamano o fecha de modificacion.
* **Mapeo en Codigo (`src/main.c`):**
  * *Implementacion:* En la funcion `archivo_requiere_sincronizacion()`, el monitor hace un `strcmp` del nombre y evalua: `if (actual->size != anteriores[i].size || actual->modification_time != anteriores[i].modification_time)`. Si da verdadero, se copia; si no, se ignora.

### C. Arquitectura Multiproceso y Tuberias (Pipes)
* **Requerimiento:** Comunicar tareas usando pipes anonimos con el comando de ejemplo: `COPIAR archivo.txt`. Repartir la carga equitativamente.
* **Mapeo en Codigo (`src/main.c` y `src/worker.c`):**
  * *Implementacion:* El monitor crea los conductos con `pipe()`, hace el `fork()` y envia el string formateado usando `write(..., "COPIAR archivo.txt")`. El Worker esta bloqueado escuchando con `read()`, limpia los primeros 7 bytes (`strncmp`) y se queda con la ruta limpia para trabajar.

### D. Creacion de Copias de Seguridad 
* **Requerimiento:** Guardar todo en `backup/` usando unicamente `open()`, `read()` y `write()`. Comandados como `cp` o `rsync` estan prohibidos.
* **Mapeo en Codigo (`src/worker.c`):**
  * *Implementacion:* La funcion `copiarArchivo()` abre el archivo origen en modo lectura (`O_RDONLY`) y crea el destino en `backup/` con los permisos `0644` e indicando que si ya existia se borre lo viejo (`O_TRUNC`). Un bucle `while(read(...) > 0)` pasa los datos byte a byte usando un buffer temporal de 4KB.

### E. Memoria Compartida y Semaforos
* **Requerimiento:** Proteger las estadisticas globales contra condiciones de carrera usando memoria compartida y semaforos POSIX.
* **Mapeo en Codigo (`src/main.c` y `src/worker.c`):**
  * *Implementacion:* Usamos `shm_open()` y `mmap()` para mapear la pizarra publica (`struct stats`). Antes de incrementar los contadores, el Worker encierra la operacion entre `sem_wait(sem)` y `sem_post(sem)`, evitando que dos Workers escriban al mismo tiempo (condicion de carrera).

### F. Servicio de Registro (Logger y FIFO)
* **Requerimiento:** Crear un proceso Logger independiente conectado a una FIFO con nombre que guarde las lineas con el formato `[fecha-hora] copiado archivo.pdf`.
* **Mapeo en Codigo (`src/logger.c` y `src/worker.c`):**
  * *Implementacion:* El Logger crea el conducto especial con `mkfifo()`. Los Workers abren la FIFO y meten un string con la fecha y hora calculadas desde la estructura del sistema `struct tm` (sumando 1900 al ano y 1 al mes). El Logger se despierta al leer bytes y los escribe al final del archivo (`O_APPEND`).

### G. Ciclo de Vida como Demonio (Daemon)
* **Requerimiento:** Convertir el monitor en un proceso invisible de fondo usando `fork()`, `setsid()` y `chdir("/")`.
* **Mapeo en Codigo (`src/main.c`):**
  * *Implementacion:* En `convertirse_en_demonio()`, el proceso padre muere con `exit`, el hijo huerfano ejecuta `setsid()` para ser lider de una sesion sin terminal, se ignora la senal de muerte del padre (`SIGHUP`) y se cierran los descriptores 0, 1 y 2 para trabajar de forma invisible de fondo.



## Manual de Operacion 
### Preparar Carpetas de Prueba
Antes de arrancar, se tiene que crear una carpeta llamada `origen` y mete un par de archivos dentro (pueden ser archivos de texto, PDFs o cualquier formato):

```bash
mkdir -p origen
echo "Contenido del archivo de pruebas 1" > origen/tarea.txt
echo "Simulacion de un documento PDF" > origen/manual.pdf
```
###  Ejecucion
Para probar el programa en consola y ver la tabla de estadisticas de sincronizacion en tiempo real:

```bash
./minisync origen
```
Una vez ejecutado, el programa creara automaticamente la carpeta backup/.

Copiara los archivos de origen a backup/ repartiendo el trabajo entre los Workers.

Se podrá ver las estadisticas consolidadas directamente en tu terminal.

Si se agrega texto a un archivo o se pone un archivo nuevo en origen, se verá que en menos de 5 segundos el programa lo detecta y lo copia sin volver a copiar lo que no ha cambiado.

Para detener este modo:  Ctrl + C

### Salida Esperada

Cuando el Monitor detecta cambios en la carpeta de origen, se inicia la distribucion por los pipes y veras un flujo como este en tu terminal (en modo interactivo):

```text
[MONITOR] Detectados 2 archivos nuevos/modificados. Lanzando 2 workers.
[MONITOR] Asignando tarea.txt al Worker 1
[MONITOR] Asignando manual.pdf al Worker 2

--- ESTADISTICAS DE SINCRONIZACION ---
Archivos copiados con exito: 2
Bytes transferidos: 58 bytes
Errores registrados: 0
--------------------------------------
```
