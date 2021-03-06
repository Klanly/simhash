
#define _CRT_SECURE_NO_WARNINGS

#include <iostream>
#include "FileUtil.h"
#include "Results.h"

//using namespace std;

#define STRLEN_FILE   16
#define STRLEN_TAG    8

bool g_bUseExt = true;

/////////////////////////////////////////////////////////////////////////////
// CResults

CResults::CResults(int nTags)
{
	m_nTags = nTags;
	m_pnSumTable = (DWORD*) calloc(nTags, sizeof(DWORD));
	memset(m_szFilePath, 0, MAX_PATH);

	m_szRowBuffer = (char*) malloc(STRLEN_FILE + 20 + (STRLEN_TAG + 6)*m_nTags);
}

CResults::~CResults()
{
	free(m_pnSumTable);
	if (m_szRowBuffer != NULL)
		free(m_szRowBuffer);
}


void CResults::NewFile(char* szFile, int nFileSize)
{
	strcpy(m_szFilePath, szFile);
	memset(m_pnSumTable, 0, m_nTags*sizeof(DWORD));
	m_nFileSize = nFileSize;
}	// CResults::NewFile


void CResults::IncrTag(int nTag)
{
	m_pnSumTable[nTag]++;
}	// CResults::IncrTag


void CResults::CloseFile()
{
	if (strlen(m_szFilePath) == 0)
		return;
	FormatRowBufferTxt();
	printf("%s", m_szRowBuffer);
}	// CResults::CloseFile


DWORD CResults::ComputeHashKey(CTags* pTags, int nIndex, bool bUseExt)
{
	DWORD dwKey = 0;
	for (int i = 0; i < m_nTags; i++)
		dwKey += (DWORD) ( ((float)m_pnSumTable[i]) * pTags->GetTag(i)->pfWeights[nIndex] );
	if (bUseExt)
	{
		float f = ((float) 0xFFFFFFFF) * HashExtension(m_szFilePath);
		dwKey = (dwKey % 0x1FFFFFFF) + ((DWORD) f);
	}
	return dwKey;
}	// CResults::ComputeHashKey


void CResults::FormatRowBufferTxt()
{
	// Make file name exactly 15 characters
	char szTruncName[MAX_PATH];
	ExtractFilename(m_szFilePath, szTruncName);
	szTruncName[STRLEN_FILE] = 0;
	int nTrunc = (int) strlen(szTruncName);
	while (nTrunc < STRLEN_FILE)
	{
		strcat(szTruncName, " ");
		nTrunc++;
	}

	// Output results row to console
	char szTag[15];
	sprintf(m_szRowBuffer, "%s  ", szTruncName);
	for (int i = 0; i < m_nTags; i++)
	{
		sprintf(szTag, ", %8d", m_pnSumTable[i]);
		strcat(m_szRowBuffer, szTag);
	}
	strcat(m_szRowBuffer, "\n");
//	printf("\n");
}	// CResults::FormatRowBufferTxt



/////////////////////////////////////////////////////////////////////////////
// CResultsSQL

CResultsSQL::CResultsSQL(int nTags) : CResults(nTags)
{
	m_pdbcon = new mysqlpp::Connection(mysqlpp::use_exceptions);
	m_pdbcon->connect(MYSQL_DATABASE, MYSQL_HOST, MYSQL_USER, MYSQL_PASS );
	m_pTags = NULL;
	m_pTransaction = NULL;
	m_szFilePath[0] = 0;
}

CResultsSQL::~CResultsSQL()
{
	delete m_pTransaction;
	m_pdbcon->close();
}


bool CResultsSQL::OpenStore(char* szName, CTags* pTags)
{
	// If the connection was not properly established with the database, error
	if ( !m_pdbcon || !m_pdbcon->connected() )
		return false;

	// Initialize members
	strcpy(m_szStoreName, szName);
	m_pTags = pTags;

	//check if the table called szName exists
	bool tableExists = false;
	mysqlpp::Query query = m_pdbcon->query();
	query << "show tables";
	mysqlpp::ResUse res = query.use();
	if (!res)
		return false; //error with query

	mysqlpp::Row row;
	try
	{
		while (row = res.fetch_row())
		{
			//printf(res.raw_value().c_str());
			const string& currentTable = row.raw_string(0);
			if (!strcmp(currentTable.c_str(), szName))
				tableExists = true;
		}
	}
	catch (const mysqlpp::EndOfResults& ){} // thrown when no more rows

	// if the table does not exist, then create it and populate the columns
	// from pTags.dwOrigTag
	// COLUMNS = (filename, directoryname, filesize, [hashkeys,] [tags,])
	if (!tableExists)
	{
		query.reset();
		query << "create table " << szName << " (filename text, ";
		query << "directoryname text, filesize int" ;
		for (int j = 0; j < pTags->GetKeyCount(); j++)
		{
			query << ", " << m_pTags->GetKeyName(j) << " text";
			if (g_bUseExt)
				query << ", " << m_pTags->GetKeyName(j) << "Ext text";
		}
		for (int i = 0; i < m_nTags; i++)
			query << ", t" << m_pTags->GetTag(i)->dwOrigTag << " int";
		query << ")";

		//printf(query.str().c_str());
		
		if (!query.execute().success)
			return false;
	}

	//start a new transaction object
	m_pTransaction = new mysqlpp::Transaction(*m_pdbcon);

	return (m_pTransaction != NULL);
}	// CResultsSQL::OpenStore


bool CResultsSQL::CommitStore()
{
	try
	{
		m_pTransaction->commit();
	}
	catch (const mysqlpp::Exception& er)
	{
		printf("CResultsSQL::CommitStore() failed: %s\n", er);
		return false;
	}
	return true;
}	// CResultsSQL::CommitStore


void CResultsSQL::NewFile(char* szFile, int nFileSize)
{	
	//initialize currentfilepath variable
	strcpy(m_szFilePath, szFile);
	CResults::NewFile(szFile, nFileSize);
}	// CResultsSQL::NewFile


void CResultsSQL::CloseFile()
{
	char szFilename[MAX_PATH];
	char szDirname[MAX_PATH];
	ExtractFilename(m_szFilePath, szFilename);
	ExtractDirname(m_szFilePath, szDirname);
	ReplaceSlashes(szDirname);

	// Insert file name, path, size
	mysqlpp::Query query = m_pdbcon->query();
	query << "insert into " << m_szStoreName;
	query << " values ('" << szFilename << "', '";
	query << szDirname << "', " << m_nFileSize;
	// Insert hashkeys
	for (int j = 0; j < m_pTags->GetKeyCount(); j++)
	{
		query << ", " << ComputeHashKey(m_pTags, j, false);
		if (g_bUseExt)
			query << ", " << ComputeHashKey(m_pTags, j, true);
	}
	// Insert sum table entries
	for (int i = 0; i < m_nTags; i++)
		query << ", " << m_pnSumTable[i];
	query << ")";
	//store row to query contents of DWORD* m_pnSumTable; and m_szFilePath;
	query.execute();
}	// CResultsSQL::CloseFile


// Returns 'false' if this directory is already associated with rows in the db
bool CResultsSQL::CheckValidDir(char* szDirname)
{
	char szSQLPath[MAX_PATH];
	strcpy(szSQLPath, szDirname);
	ReplaceSlashes(szSQLPath);

	bool dirExists = false;
	mysqlpp::Query query = m_pdbcon->query();
	query.reset();
	query << "select directoryname from " << m_szStoreName;
	query << " where directoryname= '" << szSQLPath << "'";
	mysqlpp::ResUse res = query.use();
	if (res)
	{
		mysqlpp::Row row;
		try
		{
			if (row = res.fetch_row())
			{
				dirExists = true;
				printf("'%s' has already been scanned into this table.\n", szDirname);
			}
		}
		catch (const mysqlpp::EndOfResults& ) {} // thrown when no more rows
	}
	return !dirExists;
}	// CResultsSQL::CheckValidDir



/////////////////////////////////////////////////////////////////////////////
// CResultsCSV

CResultsCSV::CResultsCSV(int nTags) : CResults(nTags)
{
	m_fp = NULL;
}

CResultsCSV::~CResultsCSV()
{
}


bool CResultsCSV::OpenStore(char* szName, CTags*)
{
	strcpy(m_szStoreName, szName);
	m_fp = fopen(szName, "wt");
	if (m_fp == NULL)
	{
		printf("CResultsCSV::OpenStore: failed to open %s\n", szName);
		return false;
	}

	return true;
}	// CResultsCSV::OpenStore


bool CResultsCSV::CommitStore()
{
	fclose(m_fp);
	m_fp = NULL;
	return true;
}	// CResultsCSV::CommitStore


void CResultsCSV::CloseFile()
{
	if (strlen(m_szFilePath) == 0)
		return;

	FormatRowBufferTxt();
	fprintf(m_fp, "%s", m_szRowBuffer);
}	// CResultsCSV::CloseFile