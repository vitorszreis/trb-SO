/*
*  myfs.c - Implementacao do sistema de arquivos MyFS
*
*  Autores: SUPER_PROGRAMADORES_C
*  Projeto: Trabalho Pratico II - Sistemas Operacionais
*  Organizacao: Universidade Federal de Juiz de Fora
*  Departamento: Dep. Ciencia da Computacao
*
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "myfs.h"
#include "vfs.h"
#include "inode.h"
#include "util.h"

//Declaracoes globais
#define MYFS_MAGIC 0x4D594653  // "MYFS" em ASCII
#define SUPERBLOCK_SECTOR 0
#define BITMAP_SECTOR 1
#define MAX_OPEN_FILES 128

// Estrutura do superbloco
typedef struct {
    unsigned int magic;
    unsigned int blockSize;
    unsigned int totalBlocks;
    unsigned int freeBlocks;
    unsigned int firstDataBlock;
    unsigned int bitmapSectors;
    unsigned int reserved[122];
} Superblock;

// Estrutura do descritor de arquivo
typedef struct {
    int inUse;
    unsigned int inumber;
    unsigned int cursor;
    Inode *inode;
} FileDescriptor;

// Variaveis globais
static Superblock *mountedSB = NULL;
static FileDescriptor fdTable[MAX_OPEN_FILES];
static Disk *mountedDisk = NULL;

// Funcoes auxiliares
static unsigned int bytesToSectors(unsigned int bytes) {
    return (bytes + DISK_SECTORDATASIZE - 1) / DISK_SECTORDATASIZE;
}

static unsigned int blockToSector(unsigned int blockNum, Superblock *sb) {
    unsigned int sectorsPerBlock = sb->blockSize / DISK_SECTORDATASIZE;
    return sb->firstDataBlock + (blockNum * sectorsPerBlock);
}

static int readBitmap(Disk *d, unsigned char *bitmap, unsigned int bitmapSize) {
    unsigned int sectorsNeeded = bytesToSectors(bitmapSize);
    unsigned char sector[DISK_SECTORDATASIZE];
    
    for (unsigned int i = 0; i < sectorsNeeded; i++) {
        if (diskReadSector(d, BITMAP_SECTOR + i, sector) < 0)
            return -1;
        
        unsigned int copySize = (bitmapSize > DISK_SECTORDATASIZE) 
                                ? DISK_SECTORDATASIZE 
                                : bitmapSize;
        memcpy(bitmap + (i * DISK_SECTORDATASIZE), sector, copySize);
        bitmapSize -= copySize;
    }
    return 0;
}

static int writeBitmap(Disk *d, unsigned char *bitmap, unsigned int bitmapSize) {
    unsigned int sectorsNeeded = bytesToSectors(bitmapSize);
    unsigned char sector[DISK_SECTORDATASIZE];
    
    for (unsigned int i = 0; i < sectorsNeeded; i++) {
        memset(sector, 0, DISK_SECTORDATASIZE);
        
        unsigned int copySize = (bitmapSize > DISK_SECTORDATASIZE) 
                                ? DISK_SECTORDATASIZE 
                                : bitmapSize;
        memcpy(sector, bitmap + (i * DISK_SECTORDATASIZE), copySize);
        
        if (diskWriteSector(d, BITMAP_SECTOR + i, sector) < 0)
            return -1;
        
        bitmapSize -= copySize;
    }
    return 0;
}

//Funcao para verificacao se o sistema de arquivos está ocioso, ou seja,
//se nao ha quisquer descritores de arquivos em uso atualmente. Retorna
//um positivo se ocioso ou, caso contrario, 0.
int myFSIsIdle (Disk *d) {
	for (int i = 0; i < MAX_OPEN_FILES; i++) {
		if (fdTable[i].inUse) {
			return 0;
		}
	}
	return 1;
}

//Funcao para formatacao de um disco com o novo sistema de arquivos
//com tamanho de blocos igual a blockSize. Retorna o numero total de
//blocos disponiveis no disco, se formatado com sucesso. Caso contrario,
//retorna -1.
int myFSFormat (Disk *d, unsigned int blockSize) {
	if (!d || blockSize < DISK_SECTORDATASIZE || blockSize % DISK_SECTORDATASIZE != 0) {
		return -1;
	}
	
	unsigned int totalSectors = diskGetNumSectors(d);
	unsigned int sectorsPerBlock = blockSize / DISK_SECTORDATASIZE;
	
	// Calcular area de i-nodes
	unsigned int inodeAreaStart = inodeAreaBeginSector();
	unsigned int inodesPerSector = inodeNumInodesPerSector();
	unsigned int inodeAreaSectors = 64;
	
	// Estimar numero de blocos
	unsigned int firstDataSector = BITMAP_SECTOR + 1 + inodeAreaSectors;
	unsigned int availableDataSectors = totalSectors - firstDataSector;
	unsigned int totalBlocks = availableDataSectors / sectorsPerBlock;
	
	// Calcular tamanho do bitmap
	unsigned int bitmapSizeBytes = (totalBlocks + 7) / 8;
	unsigned int bitmapSectors = bytesToSectors(bitmapSizeBytes);
	
	// Reajustar firstDataSector
	firstDataSector = BITMAP_SECTOR + bitmapSectors + inodeAreaSectors;
	availableDataSectors = totalSectors - firstDataSector;
	totalBlocks = availableDataSectors / sectorsPerBlock;
	bitmapSizeBytes = (totalBlocks + 7) / 8;
	
	// Criar superbloco
	Superblock sb;
	memset(&sb, 0, sizeof(Superblock));
	sb.magic = MYFS_MAGIC;
	sb.blockSize = blockSize;
	sb.totalBlocks = totalBlocks;
	sb.freeBlocks = totalBlocks;
	sb.firstDataBlock = firstDataSector;
	sb.bitmapSectors = bitmapSectors;
	
	// Escrever superbloco
	unsigned char sector[DISK_SECTORDATASIZE];
	memset(sector, 0, DISK_SECTORDATASIZE);
	memcpy(sector, &sb, sizeof(Superblock));
	
	if (diskWriteSector(d, SUPERBLOCK_SECTOR, sector) < 0) {
		return -1;
	}
	
	// Inicializar bitmap (todos livres)
	unsigned char *bitmap = calloc(bitmapSizeBytes, 1);
	if (!bitmap) {
		return -1;
	}
	
	if (writeBitmap(d, bitmap, bitmapSizeBytes) < 0) {
		free(bitmap);
		return -1;
	}
	free(bitmap);
	
	// Limpar i-nodes iniciais
	for (unsigned int i = 1; i <= 100; i++) {
		Inode *inode = inodeCreate(i, d);
		if (inode) {
			free(inode);
		}
	}
	
	return totalBlocks;
}

//Funcao para montagem/desmontagem do sistema de arquivos, se possível.
//Na montagem (x=1) e' a chance de se fazer inicializacoes, como carregar
//o superbloco na memoria. Na desmontagem (x=0), quaisquer dados pendentes
//de gravacao devem ser persistidos no disco. Retorna um positivo se a
//montagem ou desmontagem foi bem sucedida ou, caso contrario, 0.
int myFSxMount (Disk *d, int x) {
	if (x == 1) {
		// MONTAGEM
		if (!d || mountedSB != NULL) {
			return 0;
		}
		
		// Ler superbloco
		unsigned char sector[DISK_SECTORDATASIZE];
		if (diskReadSector(d, SUPERBLOCK_SECTOR, sector) < 0) {
			return 0;
		}
		
		// Alocar e copiar superbloco
		mountedSB = malloc(sizeof(Superblock));
		if (!mountedSB) {
			return 0;
		}
		memcpy(mountedSB, sector, sizeof(Superblock));
		
		// Validar numero magico
		if (mountedSB->magic != MYFS_MAGIC) {
			free(mountedSB);
			mountedSB = NULL;
			return 0;
		}
		
		// Inicializar tabela de descritores
		for (int i = 0; i < MAX_OPEN_FILES; i++) {
			fdTable[i].inUse = 0;
			fdTable[i].inumber = 0;
			fdTable[i].cursor = 0;
			fdTable[i].inode = NULL;
		}
		
		mountedDisk = d;
		return 1;
		
	} else {
		// DESMONTAGEM
		if (!mountedSB) {
			return 0;
		}
		
		if (!myFSIsIdle(d)) {
			return 0;
		}
		
		free(mountedSB);
		mountedSB = NULL;
		mountedDisk = NULL;
		
		return 1;
	}
}

//Funcao para abertura de um arquivo, a partir do caminho especificado
//em path, no disco montado especificado em d, no modo Read/Write,
//criando o arquivo se nao existir. Retorna um descritor de arquivo,
//em caso de sucesso. Retorna -1, caso contrario.
int myFSOpen (Disk *d, const char *path) {
	return -1;
}
	
//Funcao para a leitura de um arquivo, a partir de um descritor de arquivo
//existente. Os dados devem ser lidos a partir da posicao atual do cursor
//e copiados para buf. Terao tamanho maximo de nbytes. Ao fim, o cursor
//deve ter posicao atualizada para que a proxima operacao ocorra a partir
//do próximo byte apos o ultimo lido. Retorna o numero de bytes
//efetivamente lidos em caso de sucesso ou -1, caso contrario.
int myFSRead (int fd, char *buf, unsigned int nbytes) {
	return -1;
}

//Funcao para a escrita de um arquivo, a partir de um descritor de arquivo
//existente. Os dados de buf sao copiados para o disco a partir da posição
//atual do cursor e terao tamanho maximo de nbytes. Ao fim, o cursor deve
//ter posicao atualizada para que a proxima operacao ocorra a partir do
//proximo byte apos o ultimo escrito. Retorna o numero de bytes
//efetivamente escritos em caso de sucesso ou -1, caso contrario
int myFSWrite (int fd, const char *buf, unsigned int nbytes) {
	return -1;
}

//Funcao para fechar um arquivo, a partir de um descritor de arquivo
//existente. Retorna 0 caso bem sucedido, ou -1 caso contrario
int myFSClose (int fd) {
	return -1;
}

//Funcao para instalar seu sistema de arquivos no S.O., registrando-o junto
//ao virtual FS (vfs). Retorna um identificador unico (slot), caso
//o sistema de arquivos tenha sido registrado com sucesso.
//Caso contrario, retorna -1
int installMyFS (void) {
	FSInfo *fsInfo = malloc(sizeof(FSInfo));
	if (!fsInfo) {
		return -1;
	}
	
	fsInfo->fsid = 99;
	fsInfo->fsname = "MyFS";
	
	fsInfo->isidleFn = myFSIsIdle;
	fsInfo->formatFn = myFSFormat;
	fsInfo->xMountFn = myFSxMount;
	fsInfo->openFn = myFSOpen;
	fsInfo->readFn = myFSRead;
	fsInfo->writeFn = myFSWrite;
	fsInfo->closeFn = myFSClose;
	
	if (vfsRegisterFS(fsInfo) < 0) {
		free(fsInfo);
		return -1;
	}
	
	return fsInfo->fsid;
}