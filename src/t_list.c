/*
 * Copyright (c) 2009-2012, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "redis.h"

/**
    Redis_List ����REDIS_ENCODING_ZIPLIST �� REDIS_ENCODING_LINKEDLIST �����ַ�ʽ����
    �������б�ʱ Redis Ĭ��ʹ�� REDIS_ENCODING_ZIPLIST ���룬����������һ������������ʱ��
    �б�ᱻת���� REDIS_ENCODING_LINKEDLIST ���룺
    ��ͼ���б������һ���ַ���ֵ��������ַ����ĳ��ȳ���server.list_max_ziplist_value��Ĭ��ֵΪ64 ����
    ziplist �����Ľڵ㳬�� server.list_max_ziplist_entries��Ĭ��ֵΪ 512 ��
*/

void signalListAsReady(redisClient *c, robj *key);

/*-----------------------------------------------------------------------------
 * List API
 *----------------------------------------------------------------------------*/

/* Check the argument length to see if it requires us to convert the ziplist
 * to a real list. Only check raw-encoded objects because integer encoded
 * objects are never too long. */
void listTypeTryConversion(robj *subject, robj *value) {
    if (subject->encoding != REDIS_ENCODING_ZIPLIST) return;
    //����ַ���value�ĳ��ȴ���server.list_max_ziplist_value��ǿ�Ʊ���ת��
    if (value->encoding == REDIS_ENCODING_RAW &&
        sdslen(value->ptr) > server.list_max_ziplist_value)
            listTypeConvert(subject,REDIS_ENCODING_LINKEDLIST);
}

/* The function pushes an element to the specified list object 'subject',
 * at head or tail position as specified by 'where'.
 *
 * There is no need for the caller to increment the refcount of 'value' as
 * the function takes care of it if needed. */
/* ���� where �������� value �����б� subject �ı�ͷ���β
 * �����߲��ض� value ���м�������������ᴦ����
 */
void listTypePush(robj *subject, robj *value, int where) {
    /* Check if we need to convert the ziplist */
    //����Ƿ���Ҫ���б���б���ת�� ziplist -> linkedlist
    listTypeTryConversion(subject,value);
    if (subject->encoding == REDIS_ENCODING_ZIPLIST &&
        ziplistLen(subject->ptr) >= server.list_max_ziplist_entries)
            //ǿ��ת�� ziplist -> linkedlist
            listTypeConvert(subject,REDIS_ENCODING_LINKEDLIST);

    //ziplist Push
    if (subject->encoding == REDIS_ENCODING_ZIPLIST) {
        int pos = (where == REDIS_HEAD) ? ZIPLIST_HEAD : ZIPLIST_TAIL;
        value = getDecodedObject(value);
        subject->ptr = ziplistPush(subject->ptr,value->ptr,sdslen(value->ptr),pos);
        decrRefCount(value);
    } //list Push
    else if (subject->encoding == REDIS_ENCODING_LINKEDLIST) {
        if (where == REDIS_HEAD) {
            listAddNodeHead(subject->ptr,value);
        } else {
            listAddNodeTail(subject->ptr,value);
        }
        incrRefCount(value);
    } else {
        redisPanic("Unknown list encoding");
    }
}

/*���б��ͷ����β���Ƴ�������һ��Ԫ��*/
robj *listTypePop(robj *subject, int where) {
    robj *value = NULL;
    if (subject->encoding == REDIS_ENCODING_ZIPLIST) {//ziplist
        unsigned char *p;
        unsigned char *vstr;
        unsigned int vlen;
        long long vlong;
        int pos = (where == REDIS_HEAD) ? 0 : -1;
        p = ziplistIndex(subject->ptr,pos);
        if (ziplistGet(p,&vstr,&vlen,&vlong)) {
            if (vstr) {
                value = createStringObject((char*)vstr,vlen);
            } else {
                value = createStringObjectFromLongLong(vlong);
            }
            /* We only need to delete an element when it exists */
            subject->ptr = ziplistDelete(subject->ptr,&p);
        }
    } else if (subject->encoding == REDIS_ENCODING_LINKEDLIST) {//list
        list *list = subject->ptr;
        listNode *ln;
        if (where == REDIS_HEAD) {
            ln = listFirst(list);
        } else {
            ln = listLast(list);
        }
        if (ln != NULL) {
            value = listNodeValue(ln);
            incrRefCount(value);
            listDelNode(list,ln);
        }
    } else {
        redisPanic("Unknown list encoding");
    }
    return value;
}

//length
unsigned long listTypeLength(robj *subject) {
    if (subject->encoding == REDIS_ENCODING_ZIPLIST) {
        return ziplistLen(subject->ptr);
    } else if (subject->encoding == REDIS_ENCODING_LINKEDLIST) {
        return listLength((list*)subject->ptr);
    } else {
        redisPanic("Unknown list encoding");
    }
}

/* Initialize an iterator at the specified index. */
//��ʼ��list������
listTypeIterator *listTypeInitIterator(robj *subject, long index, unsigned char direction) {
    listTypeIterator *li = zmalloc(sizeof(listTypeIterator));
    li->subject = subject;
    li->encoding = subject->encoding;
    li->direction = direction;
    if (li->encoding == REDIS_ENCODING_ZIPLIST) { //ziplist
        li->zi = ziplistIndex(subject->ptr,index);
    } else if (li->encoding == REDIS_ENCODING_LINKEDLIST) { //linkedlist
        li->ln = listIndex(subject->ptr,index);
    } else {
        redisPanic("Unknown list encoding");
    }
    return li;
}

/* Clean up the iterator. */
void listTypeReleaseIterator(listTypeIterator *li) {
    zfree(li);
}

/* Stores pointer to current the entry in the provided entry structure
 * and advances the position of the iterator. Returns 1 when the current
 * entry is in fact an entry, 0 otherwise. */
//����ǰ�������Ľڵ㱣�浽 entry ��������������ָ����ǰ����һ��
int listTypeNext(listTypeIterator *li, listTypeEntry *entry) {
    /* Protect from converting when iterating */
    redisAssert(li->subject->encoding == li->encoding);

    entry->li = li;
    if (li->encoding == REDIS_ENCODING_ZIPLIST) {
        entry->zi = li->zi;
        if (entry->zi != NULL) {
            if (li->direction == REDIS_TAIL)
                li->zi = ziplistNext(li->subject->ptr,li->zi);
            else
                li->zi = ziplistPrev(li->subject->ptr,li->zi);
            return 1;
        }
    } else if (li->encoding == REDIS_ENCODING_LINKEDLIST) {
        entry->ln = li->ln;
        if (entry->ln != NULL) {
            if (li->direction == REDIS_TAIL)
                li->ln = li->ln->next;
            else
                li->ln = li->ln->prev;
            return 1;
        }
    } else {
        redisPanic("Unknown list encoding");
    }
    return 0;
}

/* Return entry or NULL at the current position of the iterator. */
//���ص�������ǰ�ڵ��ֵ����������Ѿ���ɣ����� NULL
robj *listTypeGet(listTypeEntry *entry) {
    listTypeIterator *li = entry->li;
    robj *value = NULL;
    if (li->encoding == REDIS_ENCODING_ZIPLIST) {
        unsigned char *vstr;
        unsigned int vlen;
        long long vlong;
        redisAssert(entry->zi != NULL);
        if (ziplistGet(entry->zi,&vstr,&vlen,&vlong)) {
            if (vstr) {
                value = createStringObject((char*)vstr,vlen);
            } else {
                value = createStringObjectFromLongLong(vlong);
            }
        }
    } else if (li->encoding == REDIS_ENCODING_LINKEDLIST) {
        redisAssert(entry->ln != NULL);
        value = listNodeValue(entry->ln);
        incrRefCount(value);
    } else {
        redisPanic("Unknown list encoding");
    }
    return value;
}

//���뺯����where������value�������б�Ԫ��entry֮ǰ��֮��
void listTypeInsert(listTypeEntry *entry, robj *value, int where) {
    robj *subject = entry->li->subject;
    if (entry->li->encoding == REDIS_ENCODING_ZIPLIST) {
        value = getDecodedObject(value);
        if (where == REDIS_TAIL) {//���뵽entry֮��
            unsigned char *next = ziplistNext(subject->ptr,entry->zi);

            /* When we insert after the current element, but the current element
             * is the tail of the list, we need to do a push. */
            if (next == NULL) {//��һ���ڵ�Ϊ�գ���ôziplist����zlend
                subject->ptr = ziplistPush(subject->ptr,value->ptr,sdslen(value->ptr),REDIS_TAIL);
            } else {
                subject->ptr = ziplistInsert(subject->ptr,next,value->ptr,sdslen(value->ptr));
            }
        } else {
            subject->ptr = ziplistInsert(subject->ptr,entry->zi,value->ptr,sdslen(value->ptr));
        }
        decrRefCount(value);
    } else if (entry->li->encoding == REDIS_ENCODING_LINKEDLIST) {
        if (where == REDIS_TAIL) {
            listInsertNode(subject->ptr,entry->ln,value,AL_START_TAIL);
        } else {
            listInsertNode(subject->ptr,entry->ln,value,AL_START_HEAD);
        }
        incrRefCount(value);
    } else {
        redisPanic("Unknown list encoding");
    }
}

/* Compare the given object with the entry at the current position. */
//�Ƚ��б�ڵ�entryֵ��o��ֵ�Ƿ���ͬ
int listTypeEqual(listTypeEntry *entry, robj *o) {
    listTypeIterator *li = entry->li;
    if (li->encoding == REDIS_ENCODING_ZIPLIST) {
        redisAssertWithInfo(NULL,o,o->encoding == REDIS_ENCODING_RAW);
        return ziplistCompare(entry->zi,o->ptr,sdslen(o->ptr));
    } else if (li->encoding == REDIS_ENCODING_LINKEDLIST) {
        return equalStringObjects(o,listNodeValue(entry->ln));
    } else {
        redisPanic("Unknown list encoding");
    }
}

/* Delete the element pointed to. */
void listTypeDelete(listTypeEntry *entry) {
    listTypeIterator *li = entry->li;
    if (li->encoding == REDIS_ENCODING_ZIPLIST) {
        unsigned char *p = entry->zi;
        li->subject->ptr = ziplistDelete(li->subject->ptr,&p);

        /* Update position of the iterator depending on the direction */
        if (li->direction == REDIS_TAIL)
            li->zi = p;
        else
            li->zi = ziplistPrev(li->subject->ptr,p);
    } else if (entry->li->encoding == REDIS_ENCODING_LINKEDLIST) {
        listNode *next;
        if (li->direction == REDIS_TAIL)
            next = entry->ln->next;
        else
            next = entry->ln->prev;
        listDelNode(li->subject->ptr,entry->ln);
        li->ln = next;
    } else {
        redisPanic("Unknown list encoding");
    }
}

void listTypeConvert(robj *subject, int enc) {
    listTypeIterator *li;
    listTypeEntry entry;
    redisAssertWithInfo(NULL,subject,subject->type == REDIS_LIST);

    if (enc == REDIS_ENCODING_LINKEDLIST) {
        list *l = listCreate();
        listSetFreeMethod(l,decrRefCountVoid);

        /* listTypeGet returns a robj with incremented refcount */
        li = listTypeInitIterator(subject,0,REDIS_TAIL);
        while (listTypeNext(li,&entry)) listAddNodeTail(l,listTypeGet(&entry));
        listTypeReleaseIterator(li);

        subject->encoding = REDIS_ENCODING_LINKEDLIST;
        zfree(subject->ptr);
        subject->ptr = l;
    } else {
        redisPanic("Unsupported list conversion");
    }
}

/*-----------------------------------------------------------------------------
 * List Commands
 *----------------------------------------------------------------------------*/

void pushGenericCommand(redisClient *c, int where) {
    int j, waiting = 0, pushed = 0;
    robj *lobj = lookupKeyWrite(c->db,c->argv[1]);

    //����б�Ϊ�գ���ô���������пͻ��˵ȴ�����б�
    int may_have_waiting_clients = (lobj == NULL);

    if (lobj && lobj->type != REDIS_LIST) {
        addReply(c,shared.wrongtypeerr);
        return;
    }

    // ����Ƿ��пͻ����ڵȴ�����б�
    // ����ǵĻ�����֪�������Ϳͻ��ˣ�����б��Ѿ�����
    //�� c->argv[1] ��ӵ� server.ready_keys �б���
    if (may_have_waiting_clients) signalListAsReady(c,c->argv[1]);

    for (j = 2; j < c->argc; j++) {
        c->argv[j] = tryObjectEncoding(c->argv[j]);
        if (!lobj) {
            lobj = createZiplistObject();//ʹ��ziplist����
            dbAdd(c->db,c->argv[1],lobj);
        }
        listTypePush(lobj,c->argv[j],where);
        pushed++;
    }
    addReplyLongLong(c, waiting + (lobj ? listTypeLength(lobj) : 0));
    if (pushed) {
        char *event = (where == REDIS_HEAD) ? "lpush" : "rpush";

        signalModifiedKey(c->db,c->argv[1]);
        notifyKeyspaceEvent(REDIS_NOTIFY_LIST,event,c->argv[1],c->db->id);
    }
    server.dirty += pushed;
}

/*LPUSH key value [value ...]*/
void lpushCommand(redisClient *c) {
    pushGenericCommand(c,REDIS_HEAD);
}

/*RPUSH key value [value ...]*/
void rpushCommand(redisClient *c) {
    pushGenericCommand(c,REDIS_TAIL);
}


void pushxGenericCommand(redisClient *c, robj *refval, robj *val, int where) {
    robj *subject;
    listTypeIterator *iter;
    listTypeEntry entry;
    int inserted = 0;

    //����б�key�����ڻ���Redis_List����ֱ�ӷ���
    if ((subject = lookupKeyReadOrReply(c,c->argv[1],shared.czero)) == NULL ||
        checkType(c,subject,REDIS_LIST)) return;

    if (refval != NULL) { //ִ��linsertָ�refvalΪpivot
        /* We're not sure if this value can be inserted yet, but we cannot
         * convert the list inside the iterator. We don't want to loop over
         * the list twice (once to see if the value can be inserted and once
         * to do the actual insert), so we assume this value can be inserted
         * and convert the ziplist to a regular list if necessary. */
        //�Ƿ���Ҫ����ת��
        listTypeTryConversion(subject,val);

        /* Seek refval from head to tail */
        //��ͷ��β���������Ұ���refval�Ľڵ�
        iter = listTypeInitIterator(subject,0,REDIS_TAIL);//���ɵ�����
        while (listTypeNext(iter,&entry)) { //����
            if (listTypeEqual(&entry,refval)) {
                listTypeInsert(&entry,val,where); //����ֵ��where��������refval֮ǰ��֮�����
                inserted = 1;
                break;
            }
        }
        listTypeReleaseIterator(iter); //�ͷŵ�����

        if (inserted) {//�ɹ�����һ��Ԫ�غ���Ҫ�ж��б��Ƿ���Ҫ����ת��
            /* Check if the length exceeds the ziplist length threshold. */
            if (subject->encoding == REDIS_ENCODING_ZIPLIST &&
                ziplistLen(subject->ptr) > server.list_max_ziplist_entries)
                    listTypeConvert(subject,REDIS_ENCODING_LINKEDLIST);
            signalModifiedKey(c->db,c->argv[1]);
            notifyKeyspaceEvent(REDIS_NOTIFY_LIST,"linsert",
                                c->argv[1],c->db->id);
            server.dirty++;
        } else {
            /* Notify client of a failed insert */
            addReply(c,shared.cnegone);
            return;
        }
    } else {//ִ��lpushx, rpushxָ��
        char *event = (where == REDIS_HEAD) ? "lpush" : "rpush";

        listTypePush(subject,val,where);
        signalModifiedKey(c->db,c->argv[1]);
        notifyKeyspaceEvent(REDIS_NOTIFY_LIST,event,c->argv[1],c->db->id);
        server.dirty++;
    }

    addReplyLongLong(c,listTypeLength(subject));
}

/*LPUSHX key value ���ҽ����б�key����ʱ����value�����б�key�ı�ͷ*/
void lpushxCommand(redisClient *c) {
    c->argv[2] = tryObjectEncoding(c->argv[2]);
    pushxGenericCommand(c,NULL,c->argv[2],REDIS_HEAD);
}

/*RPUSHX key value ���ҽ����б�key����ʱ����value�����б�key�ı�β*/
void rpushxCommand(redisClient *c) {
    c->argv[2] = tryObjectEncoding(c->argv[2]);
    pushxGenericCommand(c,NULL,c->argv[2],REDIS_TAIL);
}

/**LINSERT key BEFORE|AFTER pivot value
   ��ֵ value ���뵽�б� key ���У�λ��ֵ pivot ֮ǰ��֮��
   �� pivot ���������б� key ʱ����ִ���κβ���
   ���б� key ������ʱ��key ����Ϊ���б���ִ���κβ���
*/
void linsertCommand(redisClient *c) {
    c->argv[4] = tryObjectEncoding(c->argv[4]);
    if (strcasecmp(c->argv[2]->ptr,"after") == 0) {
        pushxGenericCommand(c,c->argv[3],c->argv[4],REDIS_TAIL);
    } else if (strcasecmp(c->argv[2]->ptr,"before") == 0) {
        pushxGenericCommand(c,c->argv[3],c->argv[4],REDIS_HEAD);
    } else {
        addReply(c,shared.syntaxerr);
    }
}

/*LLEN key*/
void llenCommand(redisClient *c) {
    robj *o = lookupKeyReadOrReply(c,c->argv[1],shared.czero);
    if (o == NULL || checkType(c,o,REDIS_LIST)) return;
    addReplyLongLong(c,listTypeLength(o));
}

/*LINDEX key index
  �����б� key �У��±�Ϊ index ��Ԫ��
*/
void lindexCommand(redisClient *c) {
    robj *o = lookupKeyReadOrReply(c,c->argv[1],shared.nullbulk);
    if (o == NULL || checkType(c,o,REDIS_LIST)) return;
    long index;
    robj *value = NULL;

    if ((getLongFromObjectOrReply(c, c->argv[2], &index, NULL) != REDIS_OK))
        return;

    if (o->encoding == REDIS_ENCODING_ZIPLIST) {
        unsigned char *p;
        unsigned char *vstr;
        unsigned int vlen;
        long long vlong;
        p = ziplistIndex(o->ptr,index);
        if (ziplistGet(p,&vstr,&vlen,&vlong)) {
            if (vstr) {
                value = createStringObject((char*)vstr,vlen);
            } else {
                value = createStringObjectFromLongLong(vlong);
            }
            addReplyBulk(c,value);
            decrRefCount(value);
        } else {
            addReply(c,shared.nullbulk);
        }
    } else if (o->encoding == REDIS_ENCODING_LINKEDLIST) {
        listNode *ln = listIndex(o->ptr,index);
        if (ln != NULL) {
            value = listNodeValue(ln);
            addReplyBulk(c,value);
        } else {
            addReply(c,shared.nullbulk);
        }
    } else {
        redisPanic("Unknown list encoding");
    }
}

/*LSET key index value
  ���б� key �±�Ϊ index ��Ԫ�ص�ֵ����Ϊ value
*/
void lsetCommand(redisClient *c) {
    robj *o = lookupKeyWriteOrReply(c,c->argv[1],shared.nokeyerr);
    if (o == NULL || checkType(c,o,REDIS_LIST)) return;
    long index;
    robj *value = (c->argv[3] = tryObjectEncoding(c->argv[3]));

    if ((getLongFromObjectOrReply(c, c->argv[2], &index, NULL) != REDIS_OK))
        return;

    listTypeTryConversion(o,value);
    if (o->encoding == REDIS_ENCODING_ZIPLIST) {
        unsigned char *p, *zl = o->ptr;
        p = ziplistIndex(zl,index);
        if (p == NULL) {
            addReply(c,shared.outofrangeerr);
        } else {
            o->ptr = ziplistDelete(o->ptr,&p);//ɾ��ԭ���Ľڵ�ֵ
            value = getDecodedObject(value);
            o->ptr = ziplistInsert(o->ptr,p,value->ptr,sdslen(value->ptr)); //�����µĽڵ�ֵ
            decrRefCount(value);
            addReply(c,shared.ok);
            signalModifiedKey(c->db,c->argv[1]);
            notifyKeyspaceEvent(REDIS_NOTIFY_LIST,"lset",c->argv[1],c->db->id);
            server.dirty++;
        }
    } else if (o->encoding == REDIS_ENCODING_LINKEDLIST) {
        listNode *ln = listIndex(o->ptr,index);
        if (ln == NULL) {
            addReply(c,shared.outofrangeerr);
        } else {
            decrRefCount((robj*)listNodeValue(ln));
            listNodeValue(ln) = value;
            incrRefCount(value);
            addReply(c,shared.ok);
            signalModifiedKey(c->db,c->argv[1]);
            notifyKeyspaceEvent(REDIS_NOTIFY_LIST,"lset",c->argv[1],c->db->id);
            server.dirty++;
        }
    } else {
        redisPanic("Unknown list encoding");
    }
}

void popGenericCommand(redisClient *c, int where) {
    robj *o = lookupKeyWriteOrReply(c,c->argv[1],shared.nullbulk);
    if (o == NULL || checkType(c,o,REDIS_LIST)) return;

    robj *value = listTypePop(o,where);
    if (value == NULL) {
        addReply(c,shared.nullbulk);
    } else {
        char *event = (where == REDIS_HEAD) ? "lpop" : "rpop";

        addReplyBulk(c,value);
        decrRefCount(value);
        notifyKeyspaceEvent(REDIS_NOTIFY_LIST,event,c->argv[1],c->db->id);
        if (listTypeLength(o) == 0) { //���ɾ��Ԫ�غ��б�Ϊ�գ���ôֱ��ɾ��
            notifyKeyspaceEvent(REDIS_NOTIFY_GENERIC,"del",
                                c->argv[1],c->db->id);
            dbDelete(c->db,c->argv[1]);
        }
        signalModifiedKey(c->db,c->argv[1]);
        server.dirty++;
    }
}

/*LPOP key
  �Ƴ��������б� key ��ͷԪ��
*/
void lpopCommand(redisClient *c) {
    popGenericCommand(c,REDIS_HEAD);
}

/*RPOP key
  �Ƴ��������б� key ��βԪ��
*/
void rpopCommand(redisClient *c) {
    popGenericCommand(c,REDIS_TAIL);
}

/*LRANGE key start stop
  �����б� key ��ָ�������ڵ�Ԫ�أ�������ƫ���� start �� stop ָ��
*/
void lrangeCommand(redisClient *c) {
    robj *o;
    long start, end, llen, rangelen;

    if ((getLongFromObjectOrReply(c, c->argv[2], &start, NULL) != REDIS_OK) ||
        (getLongFromObjectOrReply(c, c->argv[3], &end, NULL) != REDIS_OK)) return;

    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.emptymultibulk)) == NULL
         || checkType(c,o,REDIS_LIST)) return;

    llen = listTypeLength(o);//�б���

    /* convert negative indexes */
    if (start < 0) start = llen+start;
    if (end < 0) end = llen+end;
    if (start < 0) start = 0;

    /* Invariant: start >= 0, so this test will be true when end < 0.
     * The range is empty when start > end or start >= length. */
    if (start > end || start >= llen) {
        addReply(c,shared.emptymultibulk);
        return;
    }
    if (end >= llen) end = llen-1;
    rangelen = (end-start)+1;

    /* Return the result in form of a multi-bulk reply */
    addReplyMultiBulkLen(c,rangelen);
    if (o->encoding == REDIS_ENCODING_ZIPLIST) {
        unsigned char *p = ziplistIndex(o->ptr,start);
        unsigned char *vstr;
        unsigned int vlen;
        long long vlong;

        while(rangelen--) {
            ziplistGet(p,&vstr,&vlen,&vlong);
            if (vstr) {
                addReplyBulkCBuffer(c,vstr,vlen);
            } else {
                addReplyBulkLongLong(c,vlong);
            }
            p = ziplistNext(o->ptr,p);
        }
    } else if (o->encoding == REDIS_ENCODING_LINKEDLIST) {
        listNode *ln;

        /* If we are nearest to the end of the list, reach the element
         * starting from tail and going backward, as it is faster. */
        //����ʼλ��start�Ѿ������������һ�볤�ȣ���ôΪ�˼ӿ�����ٶȣ��ʹ�β��ͷ����
        if (start > llen/2) start -= llen;
        ln = listIndex(o->ptr,start);

        while(rangelen--) {
            addReplyBulk(c,ln->value);
            ln = ln->next;
        }
    } else {
        redisPanic("List encoding is not LINKEDLIST nor ZIPLIST!");
    }
}

/*LTRIM key start stop
  ��һ���б�����޼� (trim)������˵�����б�ֻ����ָ�������ڵ�Ԫ�أ�����ָ������֮�ڵ�Ԫ�ض�����ɾ��
*/
void ltrimCommand(redisClient *c) {
    robj *o;
    long start, end, llen, j, ltrim, rtrim;
    list *list;
    listNode *ln;

    if ((getLongFromObjectOrReply(c, c->argv[2], &start, NULL) != REDIS_OK) ||
        (getLongFromObjectOrReply(c, c->argv[3], &end, NULL) != REDIS_OK)) return;

    if ((o = lookupKeyWriteOrReply(c,c->argv[1],shared.ok)) == NULL ||
        checkType(c,o,REDIS_LIST)) return;
    llen = listTypeLength(o);

    /* convert negative indexes */
    if (start < 0) start = llen+start;
    if (end < 0) end = llen+end;
    if (start < 0) start = 0;

    /* Invariant: start >= 0, so this test will be true when end < 0.
     * The range is empty when start > end or start >= length. */
    if (start > end || start >= llen) {
        /* Out of range start or start > end result in empty list */
        ltrim = llen;
        rtrim = 0;
    } else {
        if (end >= llen) end = llen-1;
        ltrim = start;
        rtrim = llen-end-1;
    }

    /* Remove list elements to perform the trim */
    if (o->encoding == REDIS_ENCODING_ZIPLIST) {
        o->ptr = ziplistDeleteRange(o->ptr,0,ltrim);
        o->ptr = ziplistDeleteRange(o->ptr,-rtrim,rtrim);
    } else if (o->encoding == REDIS_ENCODING_LINKEDLIST) {
        list = o->ptr;
        for (j = 0; j < ltrim; j++) {
            ln = listFirst(list);
            listDelNode(list,ln);
        }
        for (j = 0; j < rtrim; j++) {
            ln = listLast(list);
            listDelNode(list,ln);
        }
    } else {
        redisPanic("Unknown list encoding");
    }

    notifyKeyspaceEvent(REDIS_NOTIFY_LIST,"ltrim",c->argv[1],c->db->id);
    if (listTypeLength(o) == 0) {
        dbDelete(c->db,c->argv[1]);
        notifyKeyspaceEvent(REDIS_NOTIFY_GENERIC,"del",c->argv[1],c->db->id);
    }
    signalModifiedKey(c->db,c->argv[1]);
    server.dirty++;
    addReply(c,shared.ok);
}

/*LREM key count value
  ���ݲ��� count ��ֵ���Ƴ��б�������� value ��ȵ�Ԫ��
  count > 0 : �ӱ�ͷ��ʼ���β�������Ƴ��� value ��ȵ�Ԫ�أ�����Ϊ count ��
  count < 0 : �ӱ�β��ʼ���ͷ�������Ƴ��� value ��ȵ�Ԫ�أ�����Ϊ count �ľ���ֵ��
  count = 0 : �Ƴ����������� value ��ȵ�ֵ��
*/
void lremCommand(redisClient *c) {
    robj *subject, *obj;
    obj = c->argv[3] = tryObjectEncoding(c->argv[3]);
    long toremove;
    long removed = 0;
    listTypeEntry entry;

    if ((getLongFromObjectOrReply(c, c->argv[2], &toremove, NULL) != REDIS_OK))
        return;

    subject = lookupKeyWriteOrReply(c,c->argv[1],shared.czero);
    if (subject == NULL || checkType(c,subject,REDIS_LIST)) return;

    /* Make sure obj is raw when we're dealing with a ziplist */
    if (subject->encoding == REDIS_ENCODING_ZIPLIST)
        obj = getDecodedObject(obj);

    listTypeIterator *li; //�б������
    if (toremove < 0) {
        toremove = -toremove;
        li = listTypeInitIterator(subject,-1,REDIS_HEAD);
    } else {
        li = listTypeInitIterator(subject,0,REDIS_TAIL);
    }

    while (listTypeNext(li,&entry)) {
        if (listTypeEqual(&entry,obj)) {
            listTypeDelete(&entry);
            server.dirty++;
            removed++;
            if (toremove && removed == toremove) break;
        }
    }
    listTypeReleaseIterator(li);

    /* Clean up raw encoded object */
    if (subject->encoding == REDIS_ENCODING_ZIPLIST)
        decrRefCount(obj);

    if (listTypeLength(subject) == 0) dbDelete(c->db,c->argv[1]);
    addReplyLongLong(c,removed);
    if (removed) signalModifiedKey(c->db,c->argv[1]);
}

/* This is the semantic of this command:
 *  RPOPLPUSH srclist dstlist:
 *    IF LLEN(srclist) > 0
 *      element = RPOP srclist
 *      LPUSH dstlist element
 *      RETURN element
 *    ELSE
 *      RETURN nil
 *    END
 *  END
 *
 * The idea is to be able to get an element from a list in a reliable way
 * since the element is not just returned but pushed against another list
 * as well. This command was originally proposed by Ezra Zygmuntowicz.
 */

void rpoplpushHandlePush(redisClient *c, robj *dstkey, robj *dstobj, robj *value) {
    /* Create the list if the key does not exist */
    if (!dstobj) {//Ŀ���б�Ϊ��
        dstobj = createZiplistObject();
        dbAdd(c->db,dstkey,dstobj);
        signalListAsReady(c,dstkey); //�� dstkey ��ӵ� server.ready_keys �б���
    }
    signalModifiedKey(c->db,dstkey);
    listTypePush(dstobj,value,REDIS_HEAD);//��ӵ��б��ײ�
    notifyKeyspaceEvent(REDIS_NOTIFY_LIST,"lpush",dstkey,c->db->id);
    /* Always send the pushed value to the client. */
    addReplyBulk(c,value);
}

/*RPOPLPUSH source destination
  ����RPOPLPUSH ��һ��ԭ��ʱ���ڣ�ִ����������������
    ���б� source �е����һ��Ԫ�� (βԪ��) �����������ظ��ͻ��ˡ�
    �� source ������Ԫ�ز��뵽�б� destination ����Ϊ destination �б�ĵ�ͷԪ�ء�
*/
void rpoplpushCommand(redisClient *c) {
    robj *sobj, *value;
    if ((sobj = lookupKeyWriteOrReply(c,c->argv[1],shared.nullbulk)) == NULL ||
        checkType(c,sobj,REDIS_LIST)) return;

    if (listTypeLength(sobj) == 0) {
        /* This may only happen after loading very old RDB files. Recent
         * versions of Redis delete keys of empty lists. */
        addReply(c,shared.nullbulk);
    } else {
        robj *dobj = lookupKeyWrite(c->db,c->argv[2]);
        robj *touchedkey = c->argv[1];

        //���ͼ��
        if (dobj && checkType(c,dobj,REDIS_LIST)) return;
        //��source�б�β��������Ԫ��
        value = listTypePop(sobj,REDIS_TAIL);
        /* We saved touched key, and protect it, since rpoplpushHandlePush
         * may change the client command argument vector (it does not
         * currently). */
        incrRefCount(touchedkey);
        //��source������Ԫ��value���뵽Ŀ���б��ͷ��
        rpoplpushHandlePush(c,c->argv[2],dobj,value);

        /* listTypePop returns an object with its refcount incremented */
        decrRefCount(value);

        /* Delete the source list when it is empty */
        notifyKeyspaceEvent(REDIS_NOTIFY_LIST,"rpop",touchedkey,c->db->id);
        if (listTypeLength(sobj) == 0) {
            dbDelete(c->db,touchedkey);
            notifyKeyspaceEvent(REDIS_NOTIFY_GENERIC,"del",
                                touchedkey,c->db->id);
        }
        signalModifiedKey(c->db,touchedkey);
        decrRefCount(touchedkey);
        server.dirty++;
    }
}

/*-----------------------------------------------------------------------------
 * Blocking POP operations
 *----------------------------------------------------------------------------*/

/* This is how the current blocking POP works, we use BLPOP as example:
 * - If the user calls BLPOP and the key exists and contains a non empty list
 *   then LPOP is called instead. So BLPOP is semantically the same as LPOP
 *   if blocking is not required.
 * - ��� BLPOP �����ã����Ҹ��� key �б�����Ҳ�Ϊ�գ���ôֱ�ӵ��� LPOP ��

 * - If instead BLPOP is called and the key does not exists or the list is
 *   empty we need to block. In order to do so we remove the notification for
 *   new data to read in the client socket (so that we'll not serve new
 *   requests if the blocking request is not served). Also we put the client
 *   in a dictionary (db->blocking_keys) mapping keys to a list of clients
 *   blocking for this keys.
 * - ��� BLPOP �����ã��� key �����ڻ��б�Ϊ�գ���ô�Կͻ��˽���������
 *   �ڶԿͻ��˽�������ʱ��ֻ�����������ݿɶ�������£�����ͻ��˷���֪ͨ��
 *   �������Ϳ�����û����������ʱ�����������ͻ��˽��д�����
 *   ���⻹��һ�� client �� key ��ӳ����ӵ������� key ��ɵ��������棬
 *   ������� key Ϊ�����������ֵ� db->blocking_keys ��

 * - If a PUSH operation against a key with blocked clients waiting is
 *   performed, we mark this key as "ready", and after the current command,
 *   MULTI/EXEC block, or script, is executed, we serve all the clients waiting
 *   for this list, from the one that blocked first, to the last, accordingly
 *   to the number of elements we have in the ready list.
 *   һ��ĳ����ɿͻ��������� key ������ PUSH ������
 *   ��ô����� key ���Ϊ���������������������/����/�ű�ִ����֮��
 *   ���������ȷ����˳�򣬴������������ key ���������Ŀͻ��ˡ�
 */

/* Set a client in blocking mode for the specified key, with the specified
 * timeout */
/*
 * ���ݸ��������� key ���Ը����ͻ��˽�������
 *
 * ������
 *  keys    ��� key
 *  numkeys key ������
 *  timeout �������ʱ��
 *  target  �ڽ������ʱ����������浽���Ŀ�� key ���󣬶����Ƿ��ظ��ͻ���
 *          ֻ���� BRPOPLPUSH ����
 *  BRPOPLPUSH source destination timeout, target ���� destination
 *
 * T = O(N)
 */
void blockForKeys(redisClient *c, robj **keys, int numkeys, time_t timeout, robj *target) {
    dictEntry *de;
    list *l;
    int j;

    c->bpop.timeout = timeout;
    c->bpop.target = target;

    if (target != NULL) incrRefCount(target);

    // ������ key ���뵽 client.bpop.keys �ֵ��O(N)
    for (j = 0; j < numkeys; j++) {
        /* If the key already exists in the dict ignore it. */
        // ��¼���� key ���ͻ���
        if (dictAdd(c->bpop.keys,keys[j],NULL) != DICT_OK) continue;
        incrRefCount(keys[j]);

        /* And in the other "side", to map keys -> clients */
        // ���������Ŀͻ�����ӵ� db->blocking_keys �ֵ��������
        de = dictFind(c->db->blocking_keys,keys[j]);
        if (de == NULL) {
            // ��� key ��һ�α�����������һ������
            int retval;

            /* For every key we take a list of clients blocked for it */
            l = listCreate();
            retval = dictAdd(c->db->blocking_keys,keys[j],l);//����������ӵ��ֵ���
            incrRefCount(keys[j]);
            redisAssertWithInfo(c,keys[j],retval == DICT_OK);
        } else {// �Ѿ��������ͻ��˱���� key ����
            l = dictGetVal(de);
        }
        listAddNodeTail(l,c);//��client��ӵ�������
    }

    /* Mark the client as a blocked client */
    c->flags |= REDIS_BLOCKED; // ���ͻ��˵�״̬����Ϊ����
    server.bpop_blocked_clients++; // Ϊ�������������ͻ���������һ
}

/* Unblock a client that's waiting in a blocking operation such as BLPOP */
//ȡ���ͻ��˵�����״̬
void unblockClientWaitingData(redisClient *c) {
    dictEntry *de;
    dictIterator *di;
    list *l;

    redisAssertWithInfo(c,NULL,dictSize(c->bpop.keys) != 0);
    // �������� key �������Ǵӿͻ��˵�c->db->blocking_keys ���������Ƴ�
    di = dictGetIterator(c->bpop.keys);
    /* The client may wait for multiple keys, so unblock it for every key. */
    while((de = dictNext(di)) != NULL) {
        robj *key = dictGetKey(de);

        /* Remove this client from the list of clients waiting for this key. */
        // ��ȡ���� key �����пͻ�������
        l = dictFetchValue(c->db->blocking_keys,key);
        redisAssertWithInfo(c,key,l != NULL);
        listDelNode(l,listSearchKey(l,c));//ɾ����������client
        /* If the list is empty we need to remove it to avoid wasting memory */
        if (listLength(l) == 0)
            dictDelete(c->db->blocking_keys,key);
    }
    dictReleaseIterator(di);

    /* Cleanup the client structure */
    //���c->bpop.keys c->target,��c->flags�޸�ΪREDIS_UNBLOCKED
    dictEmpty(c->bpop.keys);
    if (c->bpop.target) {
        decrRefCount(c->bpop.target);
        c->bpop.target = NULL;
    }
    c->flags &= ~REDIS_BLOCKED;
    c->flags |= REDIS_UNBLOCKED;
    server.bpop_blocked_clients--;
    //���ͻ�����ӵ���һ���¼� loop ǰ��Ҫȡ�������Ŀͻ����б���
    listAddNodeTail(server.unblocked_clients,c);
}

/* If the specified key has clients blocked waiting for list pushes, this
 * function will put the key reference into the server.ready_keys list.
 * Note that db->ready_keys is an hash table that allows us to avoid putting
 * the same key again and again in the list in case of multiple pushes
 * made by a script or in the context of MULTI/EXEC.
 *
 * The list will be finally processed by handleClientsBlockedOnLists() */
void signalListAsReady(redisClient *c, robj *key) {
    readyList *rl;

    /* No clients blocking for this key? No need to queue it. */
    // û�пͻ����ڵȴ���� key ��ֱ�ӷ���
    if (dictFind(c->db->blocking_keys,key) == NULL) return;

    /* Key was already signaled? No need to queue it again. */
    // key �Ѿ�λ�ھ����б�ֱ�ӷ���
    if (dictFind(c->db->ready_keys,key) != NULL) return;

    /* Ok, we need to queue this key into server.ready_keys. */
    // ��Ӱ��� key ���� db ��Ϣ�� readyList �ṹ���������˵ľ����б�
    rl = zmalloc(sizeof(*rl));
    rl->key = key;
    rl->db = c->db;
    incrRefCount(key);
    listAddNodeTail(server.ready_keys,rl);

    /* We also add the key in the db->ready_keys dictionary in order
     * to avoid adding it multiple times into a list with a simple O(1)
     * check. */
    incrRefCount(key);
    redisAssert(dictAdd(c->db->ready_keys,key,NULL) == DICT_OK);
}

/* This is an helper function for handleClientsBlockedOnLists(). It's work
 * is to serve a specific client (receiver) that is blocked on 'key'
 * in the context of the specified 'db', doing the following:
 *
 * 1) Provide the client with the 'value' element.
 * 2) If the dstkey is not NULL (we are serving a BRPOPLPUSH) also push the
 *    'value' element on the destination list (the LPUSH side of the command).
 * 3) Propagate the resulting BRPOP, BLPOP and additional LPUSH if any into
 *    the AOF and replication channel.
 *
 * The argument 'where' is REDIS_TAIL or REDIS_HEAD, and indicates if the
 * 'value' element was popped fron the head (BLPOP) or tail (BRPOP) so that
 * we can propagate the command properly.
 *
 * The function returns REDIS_OK if we are able to serve the client, otherwise
 * REDIS_ERR is returned to signal the caller that the list POP operation
 * should be undone as the client was not served: This only happens for
 * BRPOPLPUSH that fails to push the value to the destination key as it is
 * of the wrong type. */
/*
    receiver:�������Ŀͻ���
    key:��������key
    dstkey: BRPOPLPUSHָ�POP��value��ŵ��б�
    db: �ͻ�����ʹ�õ����ݿ�
    value: POP������ֵ
    where��ͷ��β
*/
int serveClientBlockedOnList(redisClient *receiver, robj *key, robj *dstkey, redisDb *db, robj *value, int where)
{
    robj *argv[3];

    if (dstkey == NULL) {
        /* Propagate the [LR]POP operation. */
        argv[0] = (where == REDIS_HEAD) ? shared.lpop :
                                          shared.rpop;
        argv[1] = key;
        //������ AOF ��ͬ���ڵ�
        propagate((where == REDIS_HEAD) ?
            server.lpopCommand : server.rpopCommand,
            db->id,argv,2,REDIS_PROPAGATE_AOF|REDIS_PROPAGATE_REPL);

        /* BRPOP/BLPOP *///������ͻ��˵Ľ��
        addReplyMultiBulkLen(receiver,2);
        addReplyBulk(receiver,key);
        addReplyBulk(receiver,value);
    } else {
        /* BRPOPLPUSH */
        robj *dstobj = lookupKeyWrite(receiver->db,dstkey);//��ȡPUSH��Ŀ���б�dstkey
        if (!(dstobj &&checkType(receiver,dstobj,REDIS_LIST)))
        {
            /* Propagate the RPOP operation. */
            argv[0] = shared.rpop;
            argv[1] = key;
            //������ AOF ��ͬ���ڵ�
            propagate(server.rpopCommand,
                db->id,argv,2,
                REDIS_PROPAGATE_AOF|
                REDIS_PROPAGATE_REPL);
            //BRPOPLPUSH��Ҫ��POP������value PUSH��Ŀ���б��У���ִ����ͨ��RPOPLPUSHָ��
            rpoplpushHandlePush(receiver,dstkey,dstobj,value);
            /* Propagate the LPUSH operation. */
            argv[0] = shared.lpush;
            argv[1] = dstkey;
            argv[2] = value;
            //������ AOF ��ͬ���ڵ�
            propagate(server.lpushCommand,
                db->id,argv,3,
                REDIS_PROPAGATE_AOF|
                REDIS_PROPAGATE_REPL);
        } else {
            /* BRPOPLPUSH failed because of wrong
             * destination type. */
            return REDIS_ERR;
        }
    }
    return REDIS_OK;
}

/* This function should be called by Redis every time a single command,
 * a MULTI/EXEC block, or a Lua script, terminated its execution after
 * being called by a client.
 *
 * All the keys with at least one client blocked that received at least
 * one new element via some PUSH operation are accumulated into
 * the server.ready_keys list. This function will run the list and will
 * serve clients accordingly. Note that the function will iterate again and
 * again as a result of serving BRPOPLPUSH we can have new blocking clients
 * to serve because of the PUSH side of BRPOPLPUSH.
 * �����б�������ĳ���ͻ��˵� key ��˵��ֻҪ��� key ��ִ����ĳ�� PUSH ����
 * ��ô��� key �ͻᱻ�ŵ� serve.ready_keys ȥ��
 *
 * ���������������� serve.ready_keys ������������� key ���д���
 *
 * ������һ����һ�εؽ��е�����
 * �������ִ�� BRPOPLPUSH ����������Ҳ����������ȡ����ȷ���±������ͻ��ˡ�
 */
void handleClientsBlockedOnLists(void) {
    while(listLength(server.ready_keys) != 0) {
        list *l;

        /* Point server.ready_keys to a fresh list and save the current one
         * locally. This way as we run the old list we are free to call
         * signalListAsReady() that may push new elements in server.ready_keys
         * when handling clients blocked into BRPOPLPUSH. */
        l = server.ready_keys; //��ȡ��ǰ��server��ready_keys
        server.ready_keys = listCreate();

        while(listLength(l) != 0) {
            listNode *ln = listFirst(l);//��ȡ��һ���ڵ�
            readyList *rl = ln->value;// ��ȡԪ�ص�ֵ��һ�������������� key �� db �� readyList

            /* First of all remove this key from db->ready_keys so that
             * we can safely call signalListAsReady() against this key. */
            //��ready��key�����ݿ�db��ready_keys�ֵ���ɾ��
            dictDelete(rl->db->ready_keys,rl->key);

            /* If the key exists and it's a list, serve blocked clients
             * with data. */
            robj *o = lookupKeyWrite(rl->db,rl->key);//�����ݿ�db�в����б�key
            if (o != NULL && o->type == REDIS_LIST) {
                dictEntry *de;

                /* We serve clients in the same order they blocked for
                 * this key, from the first blocked to the last. */
                // ȡ�������а��������б������б� key �����Ŀͻ���
                de = dictFind(rl->db->blocking_keys,rl->key);
                if (de) {
                    list *clients = dictGetVal(de);//�������Ŀͻ�������
                    int numclients = listLength(clients);

                    while(numclients--) {
                        // ȡ�������е��׸��ͻ���client
                        listNode *clientnode = listFirst(clients);
                        redisClient *receiver = clientnode->value;
                        // ���õ�����Ŀ�ֻ꣨���� BRPOPLPUSH��
                        robj *dstkey = receiver->bpop.target;
                        /**��ȡ�ϴα�������ָ��
                           ָ��������: blpopCommand, brpopCommand,brpoplpushCommand
                           where������ʶ��ͷ��βpop���б�o�е�Ԫ��
                        */
                        int where = (receiver->lastcmd &&
                                     receiver->lastcmd->proc == blpopCommand) ?
                                    REDIS_HEAD : REDIS_TAIL;
                        robj *value = listTypePop(o,where);//pop���б��е�Ԫ��

                        if (value) {//popԪ�سɹ�
                            /* Protect receiver->bpop.target, that will be
                             * freed by the next unblockClientWaitingData()
                             * call. */
                            if (dstkey) incrRefCount(dstkey);
                            // ȡ�� receiver �ͻ��˵�����״̬
                            unblockClientWaitingData(receiver);
                            // ��ֵ value ��ӵ���ɿͻ��� receiver ������ key ��
                            if (serveClientBlockedOnList(receiver,
                                rl->key,dstkey,rl->db,value,
                                where) == REDIS_ERR)
                            {
                                /* If we failed serving the client we need
                                 * to also undo the POP operation. */
                                    listTypePush(o,value,where);
                            }

                            if (dstkey) decrRefCount(dstkey);
                            decrRefCount(value);
                        } else {// ���ֿͻ���û��ȡ��ֵ��������Ȼ��Ҫ����
                            break;
                        }
                    }
                }
                //���ִ��pop���б�Ϊ�գ���ɾ��
                if (listTypeLength(o) == 0) dbDelete(rl->db,rl->key);
                /* We don't call signalModifiedKey() as it was already called
                 * when an element was pushed on the list. */
            }

            /* Free this item. */
            decrRefCount(rl->key);
            zfree(rl);
            listDelNode(l,ln);//ɾ��server.ready_keys�б��е�һ��Ԫ��
        }
        listRelease(l); /* We have the new list on place at this point. */
    }
}

int getTimeoutFromObjectOrReply(redisClient *c, robj *object, time_t *timeout) {
    long tval;

    if (getLongFromObjectOrReply(c,object,&tval,
        "timeout is not an integer or out of range") != REDIS_OK)
        return REDIS_ERR;

    if (tval < 0) {
        addReplyError(c,"timeout is negative");
        return REDIS_ERR;
    }

    if (tval > 0) tval += server.unixtime;
    *timeout = tval;

    return REDIS_OK;
}

/* Blocking RPOP/LPOP */
/**
����һ���ͻ�����Ҫִ�����²��裺
1 ���ͻ��˵�״̬��Ϊ������������������¼��������ͻ��˵ĸ��������Լ��������ʱ�ޣ�timeout�������ݡ�
2 ���ͻ��˵���Ϣ��¼�� server.db[i]->blocking_keys �У����� i Ϊ�ͻ�����ʹ�õ����ݿ���룩��
3 ����ά�ֿͻ��˺ͷ�����֮����������ӣ���������ͻ��˴����κ���Ϣ����ɿͻ���������
*/
void blockingPopGenericCommand(redisClient *c, int where) {
    robj *o;
    time_t timeout;
    int j;

    //�õ���ʱ��ʱ��
    if (getTimeoutFromObjectOrReply(c,c->argv[c->argc-1],&timeout) != REDIS_OK)
        return;

    //�ҵ���һ����Ϊ�յ��б�key����pop
    for (j = 1; j < c->argc-1; j++) {
        o = lookupKeyWrite(c->db,c->argv[j]);
        if (o != NULL) {
            if (o->type != REDIS_LIST) {
                addReply(c,shared.wrongtypeerr);
                return;
            } else {
                if (listTypeLength(o) != 0) {
                    /* Non empty list, this is like a non normal [LR]POP. */
                    char *event = (where == REDIS_HEAD) ? "lpop" : "rpop";
                    // �ǿ��б�ִ����ͨ�� [LR]POP
                    robj *value = listTypePop(o,where);
                    redisAssert(value != NULL);

                    addReplyMultiBulkLen(c,2);
                    addReplyBulk(c,c->argv[j]);
                    addReplyBulk(c,value);
                    decrRefCount(value);
                    notifyKeyspaceEvent(REDIS_NOTIFY_LIST,event,
                                        c->argv[j],c->db->id);
                    if (listTypeLength(o) == 0) {
                        dbDelete(c->db,c->argv[j]);
                        notifyKeyspaceEvent(REDIS_NOTIFY_GENERIC,"del",
                                            c->argv[j],c->db->id);
                    }
                    signalModifiedKey(c->db,c->argv[j]);
                    server.dirty++;

                    /* Replicate it as an [LR]POP instead of B[LR]POP. */
                    rewriteClientCommandVector(c,2,
                        (where == REDIS_HEAD) ? shared.lpop : shared.rpop,
                        c->argv[j]);
                    return;
                }
            }
        }
    }

    /* If we are inside a MULTI/EXEC and the list is empty the only thing
     * we can do is treating it as a timeout (even with timeout 0). */
    // ����������������������ôֻ�ܷ��صȴ���ʱ
    // ���������������������Ϊ����������һֱ�ȴ���ȥ��
    if (c->flags & REDIS_MULTI) {
        addReply(c,shared.nullmultibulk);
        return;
    }

    /* If the list is empty or the key does not exists we must block */
    // ���и��� key ��Ϊ�գ����� block
    blockForKeys(c, c->argv + 1, c->argc - 2, timeout, NULL);
}

/*BLPOP key [key ...] timeout*/
void blpopCommand(redisClient *c) {
    blockingPopGenericCommand(c,REDIS_HEAD);
}

/*BRPOP key [key ...] timeout*/
void brpopCommand(redisClient *c) {
    blockingPopGenericCommand(c,REDIS_TAIL);
}

/*BRPOPLPUSH source destination timeout*/
void brpoplpushCommand(redisClient *c) {
    time_t timeout;

    if (getTimeoutFromObjectOrReply(c,c->argv[3],&timeout) != REDIS_OK)
        return;

    robj *key = lookupKeyWrite(c->db, c->argv[1]);

    if (key == NULL) {
        if (c->flags & REDIS_MULTI) {
            /* Blocking against an empty list in a multi state
             * returns immediately. */
            addReply(c, shared.nullbulk);
        } else {
            /* The list is empty and the client blocks. */
            // ֱ�ӵȴ�Ԫ�� push �� key
            blockForKeys(c, c->argv + 1, 1, timeout, c->argv[2]);
        }
    } else {
        if (key->type != REDIS_LIST) {
            addReply(c, shared.wrongtypeerr);
        } else {
            /* The list exists and has elements, so
             * the regular rpoplpushCommand is executed. */
            redisAssertWithInfo(c,key,listTypeLength(key) > 0);
            rpoplpushCommand(c);//�б�key��������Ԫ�أ���ôִ����ͨ��rpoplpush
        }
    }
}
