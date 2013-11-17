#include "addrinfo.h"
#include "debugger.h"
#include "console.h"
#include "memory.h"
#include "sqlhelper.h"

sqlite3* userdb;

///basic database functions
void dbinit()
{
    //initialize user database
    if(sqlite3_open(":memory:", &userdb))
    {
        dputs("failed to open database!");
        return;
    }
    dbload();
    if(!sqlexec(userdb, "CREATE TABLE IF NOT EXISTS comments (id INTEGER PRIMARY KEY AUTOINCREMENT, mod TEXT, addr INT64 NOT NULL, text TEXT NOT NULL)"))
        dprintf("SQL Error: %s\n", sqllasterror());
    if(!sqlexec(userdb, "CREATE TABLE IF NOT EXISTS labels (id INTEGER PRIMARY KEY AUTOINCREMENT, mod TEXT, addr INT64 NOT NULL, text TEXT NOT NULL)"))
        dprintf("SQL Error: %s\n", sqllasterror());
    if(!sqlexec(userdb, "CREATE TABLE IF NOT EXISTS breakpoints (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT, addr INT64 NOT NULL, enabled INT NOT NULL, oldbytes INT NOT NULL, type INT NOT NULL)"))
        dprintf("SQL Error: %s\n", sqllasterror());
}

bool dbload()
{
    return sqlloadorsavedb(userdb, dbpath, false);
}

bool dbsave()
{
    CreateDirectoryA(sqlitedb_basedir, 0); //create database directory
    return sqlloadorsavedb(userdb, dbpath, true);
}

void dbclose()
{
    dbsave();
    sqlite3_db_release_memory(userdb);
    sqlite3_close(userdb); //close user database
}

///module functions
bool modnamefromaddr(uint addr, char* modname)
{
    IMAGEHLP_MODULE64 modInfo;
    memset(&modInfo, 0, sizeof(modInfo));
    modInfo.SizeOfStruct=sizeof(IMAGEHLP_MODULE64);
    if(!SymGetModuleInfo64(fdProcessInfo->hProcess, (DWORD64)addr, &modInfo))
        return false;
    _strlwr(modInfo.ModuleName);
    strcpy(modname, modInfo.ModuleName);
    return true;
}

uint modbasefromaddr(uint addr)
{
    IMAGEHLP_MODULE64 modInfo;
    memset(&modInfo, 0, sizeof(modInfo));
    modInfo.SizeOfStruct=sizeof(IMAGEHLP_MODULE64);
    if(!SymGetModuleInfo64(fdProcessInfo->hProcess, (DWORD64)addr, &modInfo))
        return false;
    return modInfo.BaseOfImage;
}

///api functions
bool apienumexports(uint base, EXPORTENUMCALLBACK cbEnum)
{
    MEMORY_BASIC_INFORMATION mbi;
    VirtualQueryEx(fdProcessInfo->hProcess, (const void*)base, &mbi, sizeof(mbi));
    uint size=mbi.RegionSize;
    void* buffer=emalloc(size, "apienumexports:buffer");
    if(!memread(fdProcessInfo->hProcess, (const void*)base, buffer, size, 0))
    {
        efree(buffer, "apienumexports:buffer");
        return false;
    }
    IMAGE_NT_HEADERS* pnth=(IMAGE_NT_HEADERS*)((uint)buffer+GetPE32DataFromMappedFile((ULONG_PTR)buffer, 0, UE_PE_OFFSET));
    uint export_dir_rva=pnth->OptionalHeader.DataDirectory[0].VirtualAddress;
    uint export_dir_size=pnth->OptionalHeader.DataDirectory[0].Size;
    efree(buffer, "apienumexports:buffer");
    IMAGE_EXPORT_DIRECTORY export_dir;
    memset(&export_dir, 0, sizeof(export_dir));
    memread(fdProcessInfo->hProcess, (const void*)(export_dir_rva+base), &export_dir, sizeof(export_dir), 0);
    unsigned int NumberOfNames=export_dir.NumberOfNames;
    if(!export_dir.NumberOfFunctions or !NumberOfNames) //no named exports
        return false;
    char modname[256]="";
    modnamefromaddr(base, modname);
    uint original_name_va=export_dir.Name+base;
    char original_name[deflen]="";
    memset(original_name, 0, sizeof(original_name));
    memread(fdProcessInfo->hProcess, (const void*)original_name_va, original_name, deflen, 0);
    char* AddrOfFunctions_va=(char*)(export_dir.AddressOfFunctions+base);
    char* AddrOfNames_va=(char*)(export_dir.AddressOfNames+base);
    char* AddrOfNameOrdinals_va=(char*)(export_dir.AddressOfNameOrdinals+base);
    for(DWORD i=0; i<NumberOfNames; i++)
    {
        DWORD curAddrOfName=0;
        memread(fdProcessInfo->hProcess, AddrOfNames_va+sizeof(DWORD)*i, &curAddrOfName, sizeof(DWORD), 0);
        char* cur_name_va=(char*)(curAddrOfName+base);
        char cur_name[deflen]="";
        memset(cur_name, 0, deflen);
        memread(fdProcessInfo->hProcess, cur_name_va, cur_name, deflen, 0);
        WORD curAddrOfNameOrdinals=0;
        memread(fdProcessInfo->hProcess, AddrOfNameOrdinals_va+sizeof(WORD)*i, &curAddrOfNameOrdinals, sizeof(WORD), 0);
        DWORD curFunctionRva=0;
        memread(fdProcessInfo->hProcess, AddrOfFunctions_va+sizeof(DWORD)*curAddrOfNameOrdinals, &curFunctionRva, sizeof(DWORD), 0);

        if(curFunctionRva>=export_dir_rva and curFunctionRva<export_dir_rva+export_dir_size)
        {
            char forwarded_api[deflen]="";
            memset(forwarded_api, 0, deflen);
            memread(fdProcessInfo->hProcess, (void*)(curFunctionRva+base), forwarded_api, deflen, 0);
            int len=strlen(forwarded_api);
            int j=0;
            while(forwarded_api[j]!='.' and j<len)
                j++;
            if(forwarded_api[j]=='.')
            {
                forwarded_api[j]=0;
                HINSTANCE hTempDll=LoadLibraryExA(forwarded_api, 0, DONT_RESOLVE_DLL_REFERENCES|LOAD_LIBRARY_AS_DATAFILE);
                if(hTempDll)
                {
                    uint local_addr=(uint)GetProcAddress(hTempDll, forwarded_api+j+1);
                    if(local_addr)
                    {
                        uint remote_addr=ImporterGetRemoteAPIAddress(fdProcessInfo->hProcess, local_addr);
                        cbEnum(base, modname, cur_name, remote_addr);
                    }
                }
            }
        }
        else
        {
            cbEnum(base, modname, cur_name, curFunctionRva+base);
        }
    }
    return true;
}

///comment functions
bool commentset(uint addr, const char* text)
{
    if(!IsFileBeingDebugged() or !memisvalidreadptr(fdProcessInfo->hProcess, addr) or !text or strlen(text)>=MAX_COMMENT_SIZE-1)
        return false;
    if(!*text) //NOTE: delete when there is no text
        return commentdel(addr);
    char commenttext[MAX_COMMENT_SIZE]="";
    sqlstringescape(text, commenttext);
    char modname[35]="";
    char sql[256]="";
    if(!modnamefromaddr(addr, modname)) //comments without module
    {
        sprintf(sql, "SELECT text FROM comments WHERE mod IS NULL AND addr=%"fext"d", addr);
        if(sqlhasresult(userdb, sql)) //there is a comment already
            sprintf(sql, "UPDATE comments SET text='%s' WHERE mod IS NULL AND addr=%"fext"d", commenttext, addr);
        else //insert
            sprintf(sql, "INSERT INTO comments (addr,text) VALUES (%"fext"d,'%s')", addr, commenttext);
    }
    else
    {
        uint modbase=modbasefromaddr(addr);
        uint rva=addr-modbase;
        sprintf(sql, "SELECT text FROM comments WHERE mod='%s' AND addr=%"fext"d", modname, rva);
        if(sqlhasresult(userdb, sql)) //there is a comment already
            sprintf(sql, "UPDATE comments SET text='%s' WHERE mod='%s' AND addr=%"fext"d", commenttext, modname, rva);
        else //insert
            sprintf(sql, "INSERT INTO comments (mod,addr,text) VALUES ('%s',%"fext"d,'%s')", modname, rva, commenttext);
    }
    if(!sqlexec(userdb, sql))
    {
        dprintf("SQL Error: %s\n", sqllasterror());
        return false;
    }
    GuiUpdateAllViews();
    dbsave();
    return true;
}

bool commentget(uint addr, char* text)
{
    if(!IsFileBeingDebugged() or !memisvalidreadptr(fdProcessInfo->hProcess, addr) or !text)
        return false;
    char modname[35]="";
    char sql[256]="";
    if(!modnamefromaddr(addr, modname)) //comments without module
        sprintf(sql, "SELECT text FROM comments WHERE mod IS NULL AND addr=%"fext"d", addr);
    else
        sprintf(sql, "SELECT text FROM comments WHERE mod='%s' AND addr=%"fext"d", modname, addr-modbasefromaddr(addr));
    return sqlgettext(userdb, sql, text);
}

bool commentdel(uint addr)
{
    if(!IsFileBeingDebugged() or !memisvalidreadptr(fdProcessInfo->hProcess, addr))
        return false;
    char modname[35]="";
    char sql[256]="";
    if(!modnamefromaddr(addr, modname)) //comments without module
        sprintf(sql, "SELECT id FROM comments WHERE mod IS NULL AND addr=%"fext"d", addr);
    else
    {
        uint modbase=modbasefromaddr(addr);
        uint rva=addr-modbase;
        sprintf(sql, "SELECT id FROM comments WHERE mod='%s' AND addr=%"fext"d", modname, rva);
    }
    int del_id=0;
    if(!sqlgetint(userdb, sql, &del_id))
        return false;
    sprintf(sql, "DELETE FROM comments WHERE id=%d", del_id);
    if(!sqlexec(userdb, sql))
    {
        dprintf("SQL Error: %s\n", sqllasterror());
        return false;
    }
    GuiUpdateAllViews();
    dbsave();
    return true;
}

///label functions
bool labelset(uint addr, const char* text)
{
    if(!IsFileBeingDebugged() or !memisvalidreadptr(fdProcessInfo->hProcess, addr) or !text or strlen(text)>=MAX_LABEL_SIZE-1)
        return false;
    if(!*text) //NOTE: delete when there is no text
        return labeldel(addr);
    char labeltext[MAX_LABEL_SIZE]="";
    sqlstringescape(text, labeltext);
    char modname[35]="";
    char sql[256]="";
    if(!modnamefromaddr(addr, modname)) //labels without module
    {
        sprintf(sql, "SELECT text FROM labels WHERE mod IS NULL AND addr=%"fext"d", addr);
        if(sqlhasresult(userdb, sql)) //there is a label already
            sprintf(sql, "UPDATE labels SET text='%s' WHERE mod IS NULL AND addr=%"fext"d", labeltext, addr);
        else //insert
            sprintf(sql, "INSERT INTO labels (addr,text) VALUES (%"fext"d,'%s')", addr, labeltext);
    }
    else
    {
        uint modbase=modbasefromaddr(addr);
        uint rva=addr-modbase;
        sprintf(sql, "SELECT text FROM labels WHERE mod='%s' AND addr=%"fext"d", modname, rva);
        if(sqlhasresult(userdb, sql)) //there is a label already
            sprintf(sql, "UPDATE labels SET text='%s' WHERE mod='%s' AND addr=%"fext"d", labeltext, modname, rva);
        else //insert
            sprintf(sql, "INSERT INTO labels (mod,addr,text) VALUES ('%s',%"fext"d,'%s')", modname, rva, labeltext);
    }
    if(!sqlexec(userdb, sql))
    {
        dprintf("SQL Error: %s\n", sqllasterror());
        return false;
    }
    GuiUpdateAllViews();
    dbsave();
    return true;
}

bool labelget(uint addr, char* text)
{
    if(!IsFileBeingDebugged() or !memisvalidreadptr(fdProcessInfo->hProcess, addr) or !text)
        return false;
    char modname[35]="";
    char sql[256]="";
    if(!modnamefromaddr(addr, modname)) //labels without module
        sprintf(sql, "SELECT text FROM labels WHERE mod IS NULL AND addr=%"fext"d", addr);
    else
        sprintf(sql, "SELECT text FROM labels WHERE mod='%s' AND addr=%"fext"d", modname, addr-modbasefromaddr(addr));
    return sqlgettext(userdb, sql, text);
}

bool labeldel(uint addr)
{
    if(!IsFileBeingDebugged() or !memisvalidreadptr(fdProcessInfo->hProcess, addr))
        return false;
    char modname[35]="";
    char sql[256]="";
    if(!modnamefromaddr(addr, modname)) //labels without module
        sprintf(sql, "SELECT id FROM labels WHERE mod IS NULL AND addr=%"fext"d", addr);
    else
    {
        uint modbase=modbasefromaddr(addr);
        uint rva=addr-modbase;
        sprintf(sql, "SELECT id FROM labels WHERE mod='%s' AND addr=%"fext"d", modname, rva);
    }
    int del_id=0;
    if(!sqlgetint(userdb, sql, &del_id))
        return false;
    sprintf(sql, "DELETE FROM labels WHERE id=%d", del_id);
    if(!sqlexec(userdb, sql))
    {
        dprintf("SQL Error: %s\n", sqllasterror());
        return false;
    }
    dbsave();
    GuiUpdateAllViews();
    return true;
}
