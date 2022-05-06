/*##############################################################################

    HPCC SYSTEMS software Copyright (C) 2012 HPCC Systems®.

    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
############################################################################## */

#include "dasess.hpp"
#include "dadfs.hpp"
#include "thexception.hpp"

#include "../hashdistrib/thhashdistrib.ipp"
#include "thkeyedjoin.ipp"
#include "jhtree.hpp"


namespace LegacyKJ
{

class CKeyedJoinMaster : public CMasterActivity
{
    IHThorKeyedJoinArg *helper;
    Owned<IDistributedFile> dataFile, indexFile;
    Owned<IFileDescriptor> dataFileDesc;
    Owned<CSlavePartMapping> dataFileMapping;
    MemoryBuffer offsetMapMb, initMb;
    bool localKey, remoteDataFiles;
    unsigned numTags;
    mptag_t tags[4];

public:
    CKeyedJoinMaster(CMasterGraphElement *info) : CMasterActivity(info, keyedJoinActivityStatistics)
    {
        helper = (IHThorKeyedJoinArg *) queryHelper();
        localKey = false;
        numTags = 0;
        tags[0] = tags[1] = tags[2] = tags[3] = TAG_NULL;
        reInit = 0 != (helper->getFetchFlags() & (FFvarfilename|FFdynamicfilename)) || (helper->getJoinFlags() & JFvarindexfilename);
        remoteDataFiles = false;
    }
    ~CKeyedJoinMaster()
    {
        unsigned i;
        for (i=0; i<4; i++)
            if (TAG_NULL != tags[i])
                container.queryJob().freeMPTag(tags[i]);
    }
    virtual void init() override
    {
        CMasterActivity::init();
        OwnedRoxieString indexFileName(helper->getIndexFileName());
        indexFile.setown(lookupReadFile(indexFileName, AccessMode::readRandom, false, false, 0 != (helper->getJoinFlags() & JFindexoptional)));

        unsigned keyReadWidth = (unsigned)container.queryJob().getWorkUnitValueInt("KJKRR", 0);
        if (!keyReadWidth || keyReadWidth>container.queryJob().querySlaves())
            keyReadWidth = container.queryJob().querySlaves();

        initMb.clear();
        initMb.append(indexFileName.get());
        if (helper->diskAccessRequired())
            numTags += 2;
        initMb.append(numTags);
        unsigned t=0;
        for (; t<numTags; t++)
        {
            tags[t] = container.queryJob().allocateMPTag();
            initMb.append(tags[t]);
        }
        bool keyHasTlk = false;
        if (indexFile)
        {
            if (!isFileKey(indexFile))
                throw MakeActivityException(this, TE_FileTypeMismatch, "Attempting to read flat file as an index: %s", indexFileName.get());
            unsigned numParts = 0;
            localKey = indexFile->queryAttributes().getPropBool("@local");

            if (container.queryLocalData() && !localKey)
                throw MakeActivityException(this, TE_FileTypeMismatch, "Keyed Join cannot be LOCAL unless supplied index is local");

            checkFormatCrc(this, indexFile, helper->getIndexFormatCrc(), helper->queryIndexRecordSize(), helper->getProjectedIndexFormatCrc(), helper->queryProjectedIndexRecordSize(), true);
            Owned<IFileDescriptor> indexFileDesc = indexFile->getFileDescriptor();
            IDistributedSuperFile *superIndex = indexFile->querySuperFile();
            unsigned superIndexWidth = 0;
            unsigned numSuperIndexSubs = 0;
            if (superIndex)
            {
                numSuperIndexSubs = superIndex->numSubFiles(true);
                bool first=true;
                // consistency check
                Owned<IDistributedFileIterator> iter = superIndex->getSubFileIterator(true);
                ForEach(*iter)
                {
                    IDistributedFile &f = iter->query();
                    unsigned np = f.numParts()-1;
                    IDistributedFilePart &part = f.queryPart(np);
                    const char *kind = part.queryAttributes().queryProp("@kind");
                    bool hasTlk = NULL != kind && 0 == stricmp("topLevelKey", kind); // if last part not tlk, then deemed local (might be singlePartKey)
                    if (first)
                    {
                        first = false;
                        keyHasTlk = hasTlk;
                        superIndexWidth = f.numParts();
                        if (keyHasTlk)
                            --superIndexWidth;
                    }
                    else
                    {
                        if (hasTlk != keyHasTlk)
                            throw MakeActivityException(this, 0, "Local/Single part keys cannot be mixed with distributed(tlk) keys in keyedjoin");
                        if (keyHasTlk && superIndexWidth != f.numParts()-1)
                            throw MakeActivityException(this, 0, "Super sub keys of different width cannot be mixed with distributed(tlk) keys in keyedjoin");
                    }
                }
                if (keyHasTlk)
                    numParts = superIndexWidth * numSuperIndexSubs;
                else
                    numParts = superIndex->numParts();
            }
            else
            {
                numParts = indexFile->numParts();
                if (numParts)
                {
                    const char *kind = indexFile->queryPart(indexFile->numParts()-1).queryAttributes().queryProp("@kind");
                    keyHasTlk = NULL != kind && 0 == stricmp("topLevelKey", kind);
                    if (keyHasTlk)
                        --numParts;
                }
            }
            if (numParts)
            {
                initMb.append(numParts);
                initMb.append(superIndexWidth); // 0 if not superIndex
                bool interleaved = superIndex && superIndex->isInterleaved();
                initMb.append(interleaved ? numSuperIndexSubs : 0);
                UnsignedArray parts;
                if (!superIndex || interleaved) // serialize first numParts parts, TLK are at end and are serialized separately.
                {
                    for (unsigned p=0; p<numParts; p++)
                        parts.append(p);
                }
                else // non-interleaved superindex
                {
                    unsigned p=0;
                    for (unsigned i=0; i<numSuperIndexSubs; i++)
                    {
                        for (unsigned kp=0; kp<superIndexWidth; kp++)
                            parts.append(p++);
                        if (keyHasTlk)
                            p++; // TLK's serialized separately.
                    }
                }
                indexFileDesc->serializeParts(initMb, parts);
                if (localKey)
                    keyHasTlk = false; // not used
                initMb.append(keyHasTlk);
                if (keyHasTlk)
                {
                    if (numSuperIndexSubs)
                        initMb.append(numSuperIndexSubs);
                    else
                        initMb.append((unsigned)1);

                    Owned<IDistributedFileIterator> iter;
                    IDistributedFile *f;
                    if (superIndex)
                    {
                        iter.setown(superIndex->getSubFileIterator(true));
                        f = &iter->query();
                    }
                    else
                        f = indexFile;
                    for (;;)
                    {
                        unsigned location;
                        OwnedIFile iFile;
                        StringBuffer filePath;
                        Owned<IFileDescriptor> fileDesc = f->getFileDescriptor();
                        Owned<IPartDescriptor> tlkDesc = fileDesc->getPart(fileDesc->numParts()-1);
                        if (!getBestFilePart(this, *tlkDesc, iFile, location, filePath, this))
                            throw MakeThorException(TE_FileNotFound, "Top level key part does not exist, for key: %s", f->queryLogicalName());
                        OwnedIFileIO iFileIO = iFile->open(IFOread);
                        assertex(iFileIO);

                        size32_t tlkSz = (size32_t)iFileIO->size();
                        initMb.append(tlkSz);
                        ::read(iFileIO, 0, tlkSz, initMb);

                        if (!iter || !iter->next())
                            break;
                        f = &iter->query();
                    }
                }
                if (helper->diskAccessRequired())
                {
                    OwnedRoxieString fetchFilename(helper->getFileName());
                    if (fetchFilename)
                    {
                        dataFile.setown(lookupReadFile(fetchFilename, AccessMode::readRandom, false, false, 0 != (helper->getFetchFlags() & FFdatafileoptional)));
                        if (dataFile)
                        {
                            if (isFileKey(dataFile))
                                throw MakeActivityException(this, 0, "Attempting to read index as a flat file: %s", fetchFilename.get());
                            if (superIndex)
                                throw MakeActivityException(this, 0, "Superkeys and full keyed joins are not supported");
                            dataFileDesc.setown(getConfiguredFileDescriptor(*dataFile));
                            void *ekey;
                            size32_t ekeylen;
                            helper->getFileEncryptKey(ekeylen,ekey);
                            bool encrypted = dataFileDesc->queryProperties().getPropBool("@encrypted");
                            if (0 != ekeylen)
                            {
                                memset(ekey,0,ekeylen);
                                free(ekey);
                                if (!encrypted)
                                {
                                    Owned<IException> e = MakeActivityWarning(&container, TE_EncryptionMismatch, "Ignoring encryption key provided as file '%s' was not published as encrypted", dataFile->queryLogicalName());
                                    queryJobChannel().fireException(e);
                                }
                            }
                            else if (encrypted)
                                throw MakeActivityException(this, 0, "File '%s' was published as encrypted but no encryption key provided", dataFile->queryLogicalName());

                            /* If fetch file is local to cluster, fetches are sent to be processed to local node, each node has info about it's
                             * local parts only.
                             * If fetch file is off cluster, fetches are performed by requesting node directly on fetch part, therefore each nodes
                             * needs all part descriptors.
                             */
                            remoteDataFiles = false;
                            RemoteFilename rfn;
                            dataFileDesc->queryPart(0)->getFilename(0, rfn);
                            if (!rfn.queryIP().ipequals(container.queryJob().querySlaveGroup().queryNode(0).endpoint()))
                                remoteDataFiles = true;
                            if (!remoteDataFiles) // local to cluster
                            {
                                unsigned dataReadWidth = (unsigned)container.queryJob().getWorkUnitValueInt("KJDRR", 0);
                                if (!dataReadWidth || dataReadWidth>container.queryJob().querySlaves())
                                    dataReadWidth = container.queryJob().querySlaves();
                                Owned<IGroup> grp = container.queryJob().querySlaveGroup().subset((unsigned)0, dataReadWidth);
                                dataFileMapping.setown(getFileSlaveMaps(dataFile->queryLogicalName(), *dataFileDesc, container.queryJob().queryUserDescriptor(), *grp, false, false, NULL));
                                dataFileMapping->serializeFileOffsetMap(offsetMapMb.clear());
                            }
                        }
                        else
                            indexFile.clear();
                    }
                }
            }
            else
                indexFile.clear();
        }
        if (!indexFile)
            initMb.append((unsigned)0);
    }
    virtual void kill() override
    {
        CMasterActivity::kill();
        indexFile.clear();
        dataFile.clear();
    }
    virtual void serializeSlaveData(MemoryBuffer &dst, unsigned slave) override
    {
        dst.append(initMb);
        if (indexFile && helper->diskAccessRequired())
        {
            if (dataFile)
            {
                dst.append(remoteDataFiles);
                if (remoteDataFiles)
                {
                    UnsignedArray parts;
                    parts.append((unsigned)-1); // weird convention meaning all
                    dst.append(dataFileDesc->numParts());
                    dataFileDesc->serializeParts(dst, parts);
                }
                else
                {
                    dataFileMapping->serializeMap(slave, dst);
                    dst.append(offsetMapMb);
                }
            }
            else
            {
                CSlavePartMapping::serializeNullMap(dst);
                CSlavePartMapping::serializeNullOffsetMap(dst);
            }
        }
    }
};


CActivityBase *createKeyedJoinActivityMaster(CMasterGraphElement *info)
{
    return new CKeyedJoinMaster(info);
}

}
