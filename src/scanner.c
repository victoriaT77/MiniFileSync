#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

// Funcion auxiliar interna para recorrer recursivamente
void recorrer_directorio_recursivo(const char *ruta_base, struct FileMetadata *lista, int *contador, int max_archivos) {
    DIR *dir = opendir(ruta_base);
    if (!dir) {
        return; // Si no se puede abrir el directorio, salimos (evitamos romper el programa)
    }

    struct dirent *entrada;
    struct stat info_archivo;
    char nueva_ruta[MAX_PATH];

    // Leer cada elemento dentro del directorio actual usando readdir()
    while ((entrada = readdir(dir)) != NULL) {
        // Ignorar de forma estricta los directorios especiales "." y ".." para evitar bucles infinitos
        if (strcmp(entrada->d_name, ".") == 0 || strcmp(entrada->d_name, "..") == 0) {
            continue;
        }

        // Construir la ruta completa del archivo/directorio encontrado
        snprintf(nueva_ruta, sizeof(nueva_ruta), "%s/%s", ruta_base, entrada->d_name);

        // Obtener los metadatos usando lstat() para manejar enlaces de forma correcta
        if (lstat(nueva_ruta, &info_archivo) == -1) {
            continue;
        }

        // Si es un directorio, entramos recursivamente
        if (S_ISDIR(info_archivo.st_mode)) {
            recorrer_directorio_recursivo(nueva_ruta, lista, contador, max_archivos);
        } 
        // Si es un archivo regular, guardamos sus metadatos
        else if (S_ISREG(info_archivo.st_mode)) {
            if (*contador >= max_archivos) {
                break; // Limite de seguridad para evitar desbordamiento de memoria
            }

            // Almacenar los datos en la estructura definida en common.h
            struct FileMetadata *meta = &lista[*contador];
            strncpy(meta->ruta, nueva_ruta, MAX_PATH);
            meta->inode = info_archivo.st_ino;
            meta->size = info_archivo.st_size;
            meta->modification_time = info_archivo.st_mtime;
            meta->permissions = info_archivo.st_mode & 0777; // Filtrar solo los bits de permisos

            (*contador)++;
        }
    }

    closedir(dir);
}

// Funcion principal expuesta para el Monitor
int escanear_directorio(const char *directorio_origen, struct FileMetadata *lista, int max_archivos) {
    int total_archivos = 0;
    recorrer_directorio_recursivo(directorio_origen, lista, &total_archivos, max_archivos);
    return total_archivos; // Retorna la cantidad de archivos encontrados
}