//-----------------------------------------------------------------------------
// Copyright (c) 2016, 2025, Oracle and/or its affiliates.
//
// This software is dual-licensed to you under the Universal Permissive License
// (UPL) 1.0 as shown at https://oss.oracle.com/licenses/upl and Apache License
// 2.0 as shown at http://www.apache.org/licenses/LICENSE-2.0. You may choose
// either license.
//
// If you elect to accept the software under the Apache License, Version 2.0,
// the following applies:
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// dpiObject.c
//   Implementation of objects.
//-----------------------------------------------------------------------------

#include "dpiImpl.h"

// forward declarations of internal functions only used in this file
int dpiObject__closeHelper(dpiObject *obj, int checkError, dpiError *error);


//-----------------------------------------------------------------------------
// dpiObject__allocate() [INTERNAL]
//   Allocate and initialize an object structure.
//-----------------------------------------------------------------------------
int dpiObject__allocate(dpiObjectType *objType, void *instance,
        void *indicator, dpiObject *dependsOnObj, dpiObject **obj,
        dpiError *error)
{
    dpiObject *tempObj;

    if (dpiGen__allocate(DPI_HTYPE_OBJECT, objType->env, (void**) &tempObj,
            error) < 0)
        return DPI_FAILURE;
    dpiGen__setRefCount(objType, error, 1);
    tempObj->type = objType;
    tempObj->instance = instance;
    tempObj->indicator = indicator;
    if (dependsOnObj) {
        dpiGen__setRefCount(dependsOnObj, error, 1);
        tempObj->dependsOnObj = dependsOnObj;
    }
    if (!instance) {
        if (dpiOci__objectNew(tempObj, error) < 0) {
            dpiObject__free(tempObj, error);
            return DPI_FAILURE;
        }
        if (dpiOci__objectGetInd(tempObj, error) < 0) {
            dpiObject__free(tempObj, error);
            return DPI_FAILURE;
        }
    }
    if (tempObj->instance && !dependsOnObj) {
        if (dpiHandleList__addHandle(objType->conn->objects, tempObj,
                &tempObj->openSlotNum, error) < 0) {
            dpiObject__free(tempObj, error);
            return DPI_FAILURE;
        }
    }
    *obj = tempObj;
    return DPI_SUCCESS;
}


//-----------------------------------------------------------------------------
// dpiObject__check() [INTERNAL]
//   Determine if the object handle provided is available for use.
//-----------------------------------------------------------------------------
static int dpiObject__check(dpiObject *obj, const char *fnName,
        dpiError *error)
{
    if (dpiGen__startPublicFn(obj, DPI_HTYPE_OBJECT, fnName, error) < 0)
        return DPI_FAILURE;
    return dpiConn__checkConnected(obj->type->conn, error);
}


//-----------------------------------------------------------------------------
// dpiObject__checkIsCollection() [INTERNAL]
//   Check if the object is a collection, and if not, raise an exception.
//-----------------------------------------------------------------------------
static int dpiObject__checkIsCollection(dpiObject *obj, const char *fnName,
        dpiError *error)
{
    if (dpiObject__check(obj, fnName, error) < 0)
        return DPI_FAILURE;
    if (!obj->type->isCollection)
        return dpiError__set(error, "check collection", DPI_ERR_NOT_COLLECTION,
                obj->type->schemaLength, obj->type->schema,
                obj->type->nameLength, obj->type->name);
    return DPI_SUCCESS;
}


//-----------------------------------------------------------------------------
// dpiObject__clearOracleValue() [INTERNAL]
//   Clear the Oracle value after use.
//-----------------------------------------------------------------------------
static void dpiObject__clearOracleValue(dpiObject *obj, dpiError *error,
        dpiOracleDataBuffer *buffer, dpiLob *lob,
        dpiOracleTypeNum oracleTypeNum)
{
    switch (oracleTypeNum) {
        case DPI_ORACLE_TYPE_CHAR:
        case DPI_ORACLE_TYPE_NCHAR:
        case DPI_ORACLE_TYPE_VARCHAR:
        case DPI_ORACLE_TYPE_NVARCHAR:
            if (buffer->asString)
                dpiOci__stringResize(obj->env->handle, &buffer->asString, 0,
                        error);
            break;
        case DPI_ORACLE_TYPE_RAW:
            if (buffer->asRawData)
                dpiOci__rawResize(obj->env->handle, &buffer->asRawData, 0,
                        error);
            break;
        case DPI_ORACLE_TYPE_TIMESTAMP:
            if (buffer->asTimestamp)
                dpiOci__descriptorFree(buffer->asTimestamp,
                        DPI_OCI_DTYPE_TIMESTAMP);
            break;
        case DPI_ORACLE_TYPE_TIMESTAMP_TZ:
            if (buffer->asTimestamp)
                dpiOci__descriptorFree(buffer->asTimestamp,
                        DPI_OCI_DTYPE_TIMESTAMP_TZ);
            break;
        case DPI_ORACLE_TYPE_TIMESTAMP_LTZ:
            if (buffer->asTimestamp)
                dpiOci__descriptorFree(buffer->asTimestamp,
                        DPI_OCI_DTYPE_TIMESTAMP_LTZ);
            break;
        case DPI_ORACLE_TYPE_CLOB:
        case DPI_ORACLE_TYPE_NCLOB:
        case DPI_ORACLE_TYPE_BLOB:
        case DPI_ORACLE_TYPE_BFILE:
            if (lob)
                dpiGen__setRefCount(lob, error, -1);
            break;
        default:
            break;
    };
}


//-----------------------------------------------------------------------------
// dpiObject__close() [INTERNAL]
//   Close the object (frees the memory for the instance). This is needed to
// avoid trying to do so after the connection which created the object is
// closed. In some future release of the Oracle Client libraries this may not
// be needed, at which point this code and all of the code for managing the
// list of objects created by a collection can be removed.
//-----------------------------------------------------------------------------
int dpiObject__close(dpiObject *obj, int checkError, dpiError *error)
{
    int closing;

    // determine whether object is already being closed and if not, mark
    // object as being closed; this MUST be done while holding the lock (if
    // in threaded mode) to avoid race conditions!
    if (obj->env->threaded)
        dpiMutex__acquire(obj->env->mutex);
    closing = obj->closing;
    obj->closing = 1;
    if (obj->env->threaded)
        dpiMutex__release(obj->env->mutex);

    // if object is already being closed, nothing needs to be done
    if (closing)
        return DPI_SUCCESS;

    // perform actual work of closing object; if this fails, reset closing
    // flag; again, this must be done while holding the lock (if in threaded
    // mode) in order to avoid race conditions!
    if (obj->instance && !obj->dependsOnObj) {
        if (dpiObject__closeHelper(obj, checkError, error) < 0) {
            if (obj->env->threaded)
                dpiMutex__acquire(obj->env->mutex);
            obj->closing = 0;
            if (obj->env->threaded)
                dpiMutex__release(obj->env->mutex);
            return DPI_FAILURE;
        }
    }

    return DPI_SUCCESS;
}


//-----------------------------------------------------------------------------
// dpiObject__closeHelper() [INTERNAL]
//   Helper function for closing an object.
//-----------------------------------------------------------------------------
int dpiObject__closeHelper(dpiObject *obj, int checkError, dpiError *error)
{
    if (dpiOci__objectFree(obj->env->handle, obj->instance, checkError,
            error) < 0)
        return DPI_FAILURE;
    obj->instance = NULL;
    if (obj->freeIndicator && dpiOci__objectFree(obj->env->handle,
            obj->indicator, checkError, error) < 0)
        return DPI_FAILURE;
    obj->indicator = NULL;
    if (!obj->type->conn->closing)
        dpiHandleList__removeHandle(obj->type->conn->objects,
                obj->openSlotNum);
    return DPI_SUCCESS;
}


//-----------------------------------------------------------------------------
// dpiObject__free() [INTERNAL]
//   Free the memory for an object.
//-----------------------------------------------------------------------------
void dpiObject__free(dpiObject *obj, dpiError *error)
{
    dpiObject__close(obj, 0, error);
    if (obj->type) {
        dpiGen__setRefCount(obj->type, error, -1);
        obj->type = NULL;
    }
    if (obj->dependsOnObj) {
        dpiGen__setRefCount(obj->dependsOnObj, error, -1);
        obj->dependsOnObj = NULL;
    }
    dpiUtils__freeMemory(obj);
}


//-----------------------------------------------------------------------------
// dpiObject__fromOracleValue() [INTERNAL]
//   Populate data from the Oracle value or return an error if this is not
// possible.
//-----------------------------------------------------------------------------
static int dpiObject__fromOracleValue(dpiObject *obj, dpiError *error,
        const dpiDataTypeInfo *typeInfo, dpiOracleData *value,
        int16_t *indicator, dpiNativeTypeNum nativeTypeNum, dpiData *data)
{
    dpiOracleTypeNum valueOracleTypeNum;
    dpiBytes *asBytes;

    // null values are immediately returned (type is irrelevant)
    if (*indicator == DPI_OCI_IND_NULL) {
        data->isNull = 1;
        return DPI_SUCCESS;
    }

    // convert all other values
    data->isNull = 0;
    valueOracleTypeNum = typeInfo->oracleTypeNum;
    switch (valueOracleTypeNum) {
        case DPI_ORACLE_TYPE_CHAR:
        case DPI_ORACLE_TYPE_NCHAR:
        case DPI_ORACLE_TYPE_VARCHAR:
        case DPI_ORACLE_TYPE_NVARCHAR:
            if (nativeTypeNum == DPI_NATIVE_TYPE_BYTES) {
                asBytes = &data->value.asBytes;
                dpiOci__stringPtr(obj->env->handle, *value->asString,
                        &asBytes->ptr);
                dpiOci__stringSize(obj->env->handle, *value->asString,
                        &asBytes->length);
                if (valueOracleTypeNum == DPI_ORACLE_TYPE_NCHAR ||
                        valueOracleTypeNum == DPI_ORACLE_TYPE_NVARCHAR)
                    asBytes->encoding = obj->env->nencoding;
                else asBytes->encoding = obj->env->encoding;
                return DPI_SUCCESS;
            }
            break;
        case DPI_ORACLE_TYPE_RAW:
            if (nativeTypeNum == DPI_NATIVE_TYPE_BYTES) {
                asBytes = &data->value.asBytes;
                dpiOci__rawPtr(obj->env->handle, *value->asRawData,
                        (void**) &asBytes->ptr);
                dpiOci__rawSize(obj->env->handle, *value->asRawData,
                        &asBytes->length);
                asBytes->encoding = NULL;
                return DPI_SUCCESS;
            }
            break;
        case DPI_ORACLE_TYPE_NATIVE_INT:
            if (nativeTypeNum == DPI_NATIVE_TYPE_INT64) {
                data->value.asInt64 = *value->asInt32;
                return DPI_SUCCESS;
            }
            break;
        case DPI_ORACLE_TYPE_NATIVE_FLOAT:
            if (nativeTypeNum == DPI_NATIVE_TYPE_FLOAT) {
                data->value.asFloat = *value->asFloat;
                return DPI_SUCCESS;
            }
            break;
        case DPI_ORACLE_TYPE_NATIVE_DOUBLE:
            if (nativeTypeNum == DPI_NATIVE_TYPE_DOUBLE) {
                data->value.asDouble = *value->asDouble;
                return DPI_SUCCESS;
            }
            break;
        case DPI_ORACLE_TYPE_NUMBER:
            if (nativeTypeNum == DPI_NATIVE_TYPE_DOUBLE)
                return dpiDataBuffer__fromOracleNumberAsDouble(&data->value,
                        error, value->asNumber);
            else if (nativeTypeNum == DPI_NATIVE_TYPE_INT64)
                return dpiDataBuffer__fromOracleNumberAsInteger(&data->value,
                        error, value->asNumber);
            else if (nativeTypeNum == DPI_NATIVE_TYPE_UINT64)
                return dpiDataBuffer__fromOracleNumberAsUnsignedInteger(
                        &data->value, error, value->asNumber);
            else if (nativeTypeNum == DPI_NATIVE_TYPE_BYTES)
                return dpiDataBuffer__fromOracleNumberAsText(&data->value,
                        obj->env, error, value->asNumber);
            break;
        case DPI_ORACLE_TYPE_DATE:
            if (nativeTypeNum == DPI_NATIVE_TYPE_TIMESTAMP)
                return dpiDataBuffer__fromOracleDate(&data->value,
                        value->asDate);
            if (nativeTypeNum == DPI_NATIVE_TYPE_DOUBLE)
                return dpiDataBuffer__fromOracleDateAsDouble(&data->value,
                        obj->env, error, value->asDate);
            break;
        case DPI_ORACLE_TYPE_TIMESTAMP:
            if (nativeTypeNum == DPI_NATIVE_TYPE_TIMESTAMP)
                return dpiDataBuffer__fromOracleTimestamp(&data->value,
                        obj->env, error, *value->asTimestamp, 0);
            if (nativeTypeNum == DPI_NATIVE_TYPE_DOUBLE)
                return dpiDataBuffer__fromOracleTimestampAsDouble(&data->value,
                        valueOracleTypeNum, obj->env, error,
                        *value->asTimestamp);
            break;
        case DPI_ORACLE_TYPE_TIMESTAMP_TZ:
        case DPI_ORACLE_TYPE_TIMESTAMP_LTZ:
            if (nativeTypeNum == DPI_NATIVE_TYPE_TIMESTAMP)
                return dpiDataBuffer__fromOracleTimestamp(&data->value,
                        obj->env, error, *value->asTimestamp, 1);
            if (nativeTypeNum == DPI_NATIVE_TYPE_DOUBLE)
                return dpiDataBuffer__fromOracleTimestampAsDouble(&data->value,
                        valueOracleTypeNum, obj->env, error,
                        *value->asTimestamp);
            break;
        case DPI_ORACLE_TYPE_OBJECT:
            if (typeInfo->objectType &&
                    nativeTypeNum == DPI_NATIVE_TYPE_OBJECT) {
                void *instance = (typeInfo->objectType->isCollection &&
                    !obj->type->isCollection) ? *value->asCollection :
                            value->asRaw;
                dpiObject *tempObj;
                if (dpiObject__allocate(typeInfo->objectType, instance,
                        indicator, obj, &tempObj, error) < 0)
                    return DPI_FAILURE;
                data->value.asObject = tempObj;
                return DPI_SUCCESS;
            }
            break;
        case DPI_ORACLE_TYPE_BOOLEAN:
            if (nativeTypeNum == DPI_NATIVE_TYPE_BOOLEAN) {
                data->value.asBoolean = *(value->asBoolean);
                return DPI_SUCCESS;
            }
            break;
        case DPI_ORACLE_TYPE_CLOB:
        case DPI_ORACLE_TYPE_NCLOB:
        case DPI_ORACLE_TYPE_BLOB:
        case DPI_ORACLE_TYPE_BFILE:
            if (nativeTypeNum == DPI_NATIVE_TYPE_LOB) {
                const dpiOracleType *lobType;
                void *tempLocator;
                dpiLob *tempLob;
                lobType = dpiOracleType__getFromNum(typeInfo->oracleTypeNum,
                        error);
                if (dpiLob__allocate(obj->type->conn, lobType, &tempLob,
                        error) < 0)
                    return DPI_FAILURE;
                tempLocator = tempLob->locator;
                tempLob->locator = *(value->asLobLocator);
                if (dpiOci__lobLocatorAssign(tempLob, &tempLocator,
                        error) < 0) {
                    tempLob->locator = tempLocator;
                    dpiLob__free(tempLob, error);
                    return DPI_FAILURE;
                }
                tempLob->locator = tempLocator;
                data->value.asLOB = tempLob;
                return DPI_SUCCESS;
            }
            break;
        default:
            break;
    };

    return dpiError__set(error, "from Oracle value",
            DPI_ERR_UNHANDLED_CONVERSION, valueOracleTypeNum, nativeTypeNum);
}


//-----------------------------------------------------------------------------
// dpiObject__toOracleValue() [INTERNAL]
//   Convert value from external type to the OCI data type required.
//-----------------------------------------------------------------------------
static int dpiObject__toOracleValue(dpiObject *obj, dpiError *error,
        const dpiDataTypeInfo *dataTypeInfo, dpiOracleDataBuffer *buffer,
        dpiLob **lob, void **ociValue, int16_t *valueIndicator,
        void **objectIndicator, dpiNativeTypeNum nativeTypeNum, dpiData *data)
{
    dpiOracleTypeNum valueOracleTypeNum;
    uint32_t handleType;
    dpiObject *otherObj;
    dpiBytes *bytes;

    // nulls are handled easily
    *objectIndicator = NULL;
    if (data->isNull) {
        *ociValue = NULL;
        *valueIndicator = DPI_OCI_IND_NULL;
        buffer->asRaw = NULL;
        return DPI_SUCCESS;
    }

    // convert all other values
    *valueIndicator = DPI_OCI_IND_NOTNULL;
    valueOracleTypeNum = dataTypeInfo->oracleTypeNum;
    switch (valueOracleTypeNum) {
        case DPI_ORACLE_TYPE_CHAR:
        case DPI_ORACLE_TYPE_NCHAR:
        case DPI_ORACLE_TYPE_VARCHAR:
        case DPI_ORACLE_TYPE_NVARCHAR:
            buffer->asString = NULL;
            if (nativeTypeNum == DPI_NATIVE_TYPE_BYTES) {
                bytes = &data->value.asBytes;
                if (dpiOci__stringAssignText(obj->env->handle, bytes->ptr,
                        bytes->length, &buffer->asString, error) < 0)
                    return DPI_FAILURE;
                *ociValue = buffer->asString;
                return DPI_SUCCESS;
            }
            break;
        case DPI_ORACLE_TYPE_RAW:
            buffer->asRawData = NULL;
            if (nativeTypeNum == DPI_NATIVE_TYPE_BYTES) {
                bytes = &data->value.asBytes;
                if (dpiOci__rawAssignBytes(obj->env->handle, bytes->ptr,
                        bytes->length, &buffer->asRawData, error) < 0)
                    return DPI_FAILURE;
                *ociValue = buffer->asRawData;
                return DPI_SUCCESS;
            }
            break;
        case DPI_ORACLE_TYPE_NATIVE_INT:
            if (nativeTypeNum == DPI_NATIVE_TYPE_INT64) {
                buffer->asInt32 = (int32_t) data->value.asInt64;
                *ociValue = &buffer->asInt32;
                return DPI_SUCCESS;
            }
            break;
        case DPI_ORACLE_TYPE_NUMBER:
            *ociValue = &buffer->asNumber;
            if (nativeTypeNum == DPI_NATIVE_TYPE_INT64)
                return dpiDataBuffer__toOracleNumberFromInteger(&data->value,
                        error, &buffer->asNumber);
            if (nativeTypeNum == DPI_NATIVE_TYPE_DOUBLE)
                return dpiDataBuffer__toOracleNumberFromDouble(&data->value,
                        error, &buffer->asNumber);
            if (nativeTypeNum == DPI_NATIVE_TYPE_BYTES)
                return dpiDataBuffer__toOracleNumberFromText(&data->value,
                        obj->env, error, &buffer->asNumber);
            break;
        case DPI_ORACLE_TYPE_NATIVE_FLOAT:
            if (nativeTypeNum == DPI_NATIVE_TYPE_FLOAT) {
                buffer->asFloat = data->value.asFloat;
                *ociValue = &buffer->asFloat;
                return DPI_SUCCESS;
            } else if (nativeTypeNum == DPI_NATIVE_TYPE_DOUBLE) {
                buffer->asFloat = (float) data->value.asDouble;
                *ociValue = &buffer->asFloat;
                return DPI_SUCCESS;
            }
            break;
        case DPI_ORACLE_TYPE_NATIVE_DOUBLE:
            if (nativeTypeNum == DPI_NATIVE_TYPE_DOUBLE) {
                buffer->asDouble = data->value.asDouble;
                *ociValue = &buffer->asDouble;
                return DPI_SUCCESS;
            }
            break;
        case DPI_ORACLE_TYPE_DATE:
            *ociValue = &buffer->asDate;
            if (nativeTypeNum == DPI_NATIVE_TYPE_TIMESTAMP)
                return dpiDataBuffer__toOracleDate(&data->value,
                        &buffer->asDate);
            if (nativeTypeNum == DPI_NATIVE_TYPE_DOUBLE)
                return dpiDataBuffer__toOracleDateFromDouble(&data->value,
                        obj->env, error, &buffer->asDate);
            break;
        case DPI_ORACLE_TYPE_TIMESTAMP:
        case DPI_ORACLE_TYPE_TIMESTAMP_TZ:
        case DPI_ORACLE_TYPE_TIMESTAMP_LTZ:
            buffer->asTimestamp = NULL;
            if (nativeTypeNum == DPI_NATIVE_TYPE_TIMESTAMP ||
                    nativeTypeNum == DPI_NATIVE_TYPE_DOUBLE) {
                if (valueOracleTypeNum == DPI_ORACLE_TYPE_TIMESTAMP_LTZ) {
                    handleType = DPI_OCI_DTYPE_TIMESTAMP_LTZ;
                } else if (valueOracleTypeNum == DPI_ORACLE_TYPE_TIMESTAMP) {
                    handleType = DPI_OCI_DTYPE_TIMESTAMP;
                } else {
                    handleType = DPI_OCI_DTYPE_TIMESTAMP_TZ;
                }
                if (dpiOci__descriptorAlloc(obj->env->handle,
                        &buffer->asTimestamp, handleType, "allocate timestamp",
                        error) < 0)
                    return DPI_FAILURE;
                *ociValue = buffer->asTimestamp;
                if (nativeTypeNum == DPI_NATIVE_TYPE_TIMESTAMP)
                    return dpiDataBuffer__toOracleTimestamp(&data->value,
                            obj->env, error, buffer->asTimestamp,
                            (valueOracleTypeNum != DPI_ORACLE_TYPE_TIMESTAMP));
                return dpiDataBuffer__toOracleTimestampFromDouble(&data->value,
                        valueOracleTypeNum, obj->env, error,
                        buffer->asTimestamp);
            }
            break;
        case DPI_ORACLE_TYPE_OBJECT:
            otherObj = data->value.asObject;
            if (nativeTypeNum == DPI_NATIVE_TYPE_OBJECT) {
                if (otherObj->type->tdo != dataTypeInfo->objectType->tdo)
                    return dpiError__set(error, "check type",
                            DPI_ERR_WRONG_TYPE, otherObj->type->schemaLength,
                            otherObj->type->schema, otherObj->type->nameLength,
                            otherObj->type->name,
                            dataTypeInfo->objectType->schemaLength,
                            dataTypeInfo->objectType->schema,
                            dataTypeInfo->objectType->nameLength,
                            dataTypeInfo->objectType->name);
                *ociValue = otherObj->instance;
                *objectIndicator = otherObj->indicator;
                return DPI_SUCCESS;
            }
            break;
        case DPI_ORACLE_TYPE_BOOLEAN:
            if (nativeTypeNum == DPI_NATIVE_TYPE_BOOLEAN) {
                buffer->asBoolean = data->value.asBoolean;
                *ociValue = &buffer->asBoolean;
                return DPI_SUCCESS;
            }
            break;
        case DPI_ORACLE_TYPE_CLOB:
        case DPI_ORACLE_TYPE_NCLOB:
        case DPI_ORACLE_TYPE_BLOB:
        case DPI_ORACLE_TYPE_BFILE:
            buffer->asLobLocator = NULL;
            if (nativeTypeNum == DPI_NATIVE_TYPE_LOB) {
                *ociValue = data->value.asLOB->locator;
                return DPI_SUCCESS;
            } else if (nativeTypeNum == DPI_NATIVE_TYPE_BYTES) {
                const dpiOracleType *lobType;
                lobType = dpiOracleType__getFromNum(valueOracleTypeNum, error);
                if (dpiLob__allocate(obj->type->conn, lobType, lob, error) < 0)
                    return DPI_FAILURE;
                bytes = &data->value.asBytes;
                if (dpiLob__setFromBytes(*lob, bytes->ptr, bytes->length,
                        error) < 0)
                    return DPI_FAILURE;
                buffer->asLobLocator = (*lob)->locator;
                *ociValue = (*lob)->locator;
                return DPI_SUCCESS;
            }
            break;

        default:
            break;
    }

    return dpiError__set(error, "to Oracle value",
            DPI_ERR_UNHANDLED_CONVERSION, valueOracleTypeNum, nativeTypeNum);
}


//-----------------------------------------------------------------------------
// dpiObject_addRef() [PUBLIC]
//   Add a reference to the object.
//-----------------------------------------------------------------------------
int dpiObject_addRef(dpiObject *obj)
{
    return dpiGen__addRef(obj, DPI_HTYPE_OBJECT, __func__);
}


//-----------------------------------------------------------------------------
// dpiObject_appendElement() [PUBLIC]
//   Append an element to the collection.
//-----------------------------------------------------------------------------
int dpiObject_appendElement(dpiObject *obj, dpiNativeTypeNum nativeTypeNum,
        dpiData *data)
{
    dpiOracleDataBuffer valueBuffer;
    int16_t scalarValueIndicator;
    dpiLob *lob = NULL;
    void *indicator;
    dpiError error;
    void *ociValue;
    int status;

    if (dpiObject__checkIsCollection(obj, __func__, &error) < 0)
        return dpiGen__endPublicFn(obj, DPI_FAILURE, &error);
    DPI_CHECK_PTR_NOT_NULL(obj, data)
    status = dpiObject__toOracleValue(obj, &error, &obj->type->elementTypeInfo,
            &valueBuffer, &lob, &ociValue, &scalarValueIndicator,
            (void**) &indicator, nativeTypeNum, data);
    if (status == DPI_SUCCESS) {
        if (!indicator)
            indicator = &scalarValueIndicator;
        status = dpiOci__collAppend(obj->type->conn, ociValue, indicator,
                obj->instance, &error);
    }
    dpiObject__clearOracleValue(obj, &error, &valueBuffer, lob,
            obj->type->elementTypeInfo.oracleTypeNum);
    return dpiGen__endPublicFn(obj, status, &error);
}


//-----------------------------------------------------------------------------
// dpiObject_copy() [PUBLIC]
//   Create a copy of the object and return it. Return NULL upon error.
//-----------------------------------------------------------------------------
int dpiObject_copy(dpiObject *obj, dpiObject **copiedObj)
{
    dpiObject *tempObj;
    dpiError error;

    if (dpiObject__check(obj, __func__, &error) < 0)
        return DPI_FAILURE;
    DPI_CHECK_PTR_NOT_NULL(obj, copiedObj)
    if (dpiObject__allocate(obj->type, NULL, NULL, NULL, &tempObj, &error) < 0)
        return dpiGen__endPublicFn(obj, DPI_FAILURE, &error);
    if (dpiOci__objectCopy(tempObj, obj->instance, obj->indicator,
            &error) < 0) {
        dpiObject__free(tempObj, &error);
        return dpiGen__endPublicFn(obj, DPI_FAILURE, &error);
    }
    *copiedObj = tempObj;
    return dpiGen__endPublicFn(obj, DPI_SUCCESS, &error);
}


//-----------------------------------------------------------------------------
// dpiObject_deleteElementByIndex() [PUBLIC]
//   Delete the element at the specified index in the collection.
//-----------------------------------------------------------------------------
int dpiObject_deleteElementByIndex(dpiObject *obj, int32_t index)
{
    dpiError error;
    int status;

    if (dpiObject__checkIsCollection(obj, __func__, &error) < 0)
        return dpiGen__endPublicFn(obj, DPI_FAILURE, &error);
    status = dpiOci__tableDelete(obj, index, &error);
    return dpiGen__endPublicFn(obj, status, &error);
}


//-----------------------------------------------------------------------------
// dpiObject_getAttributeValue() [PUBLIC]
//   Get the value of the given attribute from the object.
//-----------------------------------------------------------------------------
int dpiObject_getAttributeValue(dpiObject *obj, dpiObjectAttr *attr,
        dpiNativeTypeNum nativeTypeNum, dpiData *data)
{
    int16_t scalarValueIndicator;
    void *valueIndicator, *tdo;
    dpiOracleData value;
    dpiError error;
    int status;

    // validate parameters
    if (dpiObject__check(obj, __func__, &error) < 0)
        return DPI_FAILURE;
    DPI_CHECK_PTR_NOT_NULL(obj, data)
    if (dpiGen__checkHandle(attr, DPI_HTYPE_OBJECT_ATTR, "get attribute value",
            &error) < 0)
        return dpiGen__endPublicFn(obj, DPI_FAILURE, &error);
    if (attr->belongsToType->tdo != obj->type->tdo) {
        dpiError__set(&error, "get attribute value", DPI_ERR_WRONG_ATTR,
                attr->nameLength, attr->name, obj->type->schemaLength,
                obj->type->schema, obj->type->nameLength, obj->type->name);
        return dpiGen__endPublicFn(obj, DPI_FAILURE, &error);
    }

    // get attribute value
    if (dpiOci__objectGetAttr(obj, attr, &scalarValueIndicator,
            &valueIndicator, &value.asRaw, &tdo, &error) < 0)
        return dpiGen__endPublicFn(obj, DPI_FAILURE, &error);

    // determine the proper null indicator
    if (!valueIndicator)
        valueIndicator = &scalarValueIndicator;

    // check to see if type is supported
    if (!attr->typeInfo.oracleTypeNum) {
        dpiError__set(&error, "get attribute value",
                DPI_ERR_UNHANDLED_DATA_TYPE, attr->typeInfo.ociTypeCode);
        return dpiGen__endPublicFn(obj, DPI_FAILURE, &error);
    }

    // convert to output data format
    status = dpiObject__fromOracleValue(obj, &error, &attr->typeInfo, &value,
            (int16_t*) valueIndicator, nativeTypeNum, data);
    return dpiGen__endPublicFn(obj, status, &error);
}


//-----------------------------------------------------------------------------
// dpiObject_getElementExistsByIndex() [PUBLIC]
//   Return boolean indicating if an element exists in the collection at the
// specified index.
//-----------------------------------------------------------------------------
int dpiObject_getElementExistsByIndex(dpiObject *obj, int32_t index,
        int *exists)
{
    dpiError error;
    int status;

    if (dpiObject__checkIsCollection(obj, __func__, &error) < 0)
        return dpiGen__endPublicFn(obj, DPI_FAILURE, &error);
    DPI_CHECK_PTR_NOT_NULL(obj, exists)
    status = dpiOci__tableExists(obj, index, exists, &error);
    return dpiGen__endPublicFn(obj, status, &error);
}


//-----------------------------------------------------------------------------
// dpiObject_getElementValueByIndex() [PUBLIC]
//   Return the element at the given index in the collection.
//-----------------------------------------------------------------------------
int dpiObject_getElementValueByIndex(dpiObject *obj, int32_t index,
        dpiNativeTypeNum nativeTypeNum, dpiData *data)
{
    dpiOracleData value;
    int exists, status;
    void *indicator;
    dpiError error;

    if (dpiObject__checkIsCollection(obj, __func__, &error) < 0)
        return dpiGen__endPublicFn(obj, DPI_FAILURE, &error);
    DPI_CHECK_PTR_NOT_NULL(obj, data)
    if (dpiOci__collGetElem(obj->type->conn, obj->instance, index, &exists,
            &value.asRaw, &indicator, &error) < 0)
        return dpiGen__endPublicFn(obj, DPI_FAILURE, &error);
    if (!exists) {
        dpiError__set(&error, "get element value", DPI_ERR_INVALID_INDEX,
                index);
        return dpiGen__endPublicFn(obj, DPI_FAILURE, &error);
    }
    status = dpiObject__fromOracleValue(obj, &error,
            &obj->type->elementTypeInfo, &value, (int16_t*) indicator,
            nativeTypeNum, data);
    return dpiGen__endPublicFn(obj, status, &error);
}


//-----------------------------------------------------------------------------
// dpiObject_getFirstIndex() [PUBLIC]
//   Return the index of the first entry in the collection.
//-----------------------------------------------------------------------------
int dpiObject_getFirstIndex(dpiObject *obj, int32_t *index, int *exists)
{
    dpiError error;
    int32_t size;
    int status;

    if (dpiObject__checkIsCollection(obj, __func__, &error) < 0)
        return dpiGen__endPublicFn(obj, DPI_FAILURE, &error);
    DPI_CHECK_PTR_NOT_NULL(obj, index)
    DPI_CHECK_PTR_NOT_NULL(obj, exists)
    if (dpiOci__tableSize(obj, &size, &error) < 0)
        return dpiGen__endPublicFn(obj, DPI_FAILURE, &error);
    *exists = (size != 0);
    if (*exists)
        status = dpiOci__tableFirst(obj, index, &error);
    else status = DPI_SUCCESS;
    return dpiGen__endPublicFn(obj, status, &error);
}


//-----------------------------------------------------------------------------
// dpiObject_getLastIndex() [PUBLIC]
//   Return the index of the last entry in the collection.
//-----------------------------------------------------------------------------
int dpiObject_getLastIndex(dpiObject *obj, int32_t *index, int *exists)
{
    dpiError error;
    int32_t size;
    int status;

    if (dpiObject__checkIsCollection(obj, __func__, &error) < 0)
        return dpiGen__endPublicFn(obj, DPI_FAILURE, &error);
    DPI_CHECK_PTR_NOT_NULL(obj, index)
    DPI_CHECK_PTR_NOT_NULL(obj, exists)
    if (dpiOci__tableSize(obj, &size, &error) < 0)
        return dpiGen__endPublicFn(obj, DPI_FAILURE, &error);
    *exists = (size != 0);
    if (*exists)
        status = dpiOci__tableLast(obj, index, &error);
    else status = DPI_SUCCESS;
    return dpiGen__endPublicFn(obj, status, &error);
}


//-----------------------------------------------------------------------------
// dpiObject_getNextIndex() [PUBLIC]
//   Return the index of the next entry in the collection following the index
// specified. If there is no next entry, exists is set to 0.
//-----------------------------------------------------------------------------
int dpiObject_getNextIndex(dpiObject *obj, int32_t index, int32_t *nextIndex,
        int *exists)
{
    dpiError error;
    int status;

    if (dpiObject__checkIsCollection(obj, __func__, &error) < 0)
        return dpiGen__endPublicFn(obj, DPI_FAILURE, &error);
    DPI_CHECK_PTR_NOT_NULL(obj, nextIndex)
    DPI_CHECK_PTR_NOT_NULL(obj, exists)
    status = dpiOci__tableNext(obj, index, nextIndex, exists, &error);
    return dpiGen__endPublicFn(obj, status, &error);
}


//-----------------------------------------------------------------------------
// dpiObject_getPrevIndex() [PUBLIC]
//   Return the index of the previous entry in the collection preceding the
// index specified. If there is no previous entry, exists is set to 0.
//-----------------------------------------------------------------------------
int dpiObject_getPrevIndex(dpiObject *obj, int32_t index, int32_t *prevIndex,
        int *exists)
{
    dpiError error;
    int status;

    if (dpiObject__checkIsCollection(obj, __func__, &error) < 0)
        return dpiGen__endPublicFn(obj, DPI_FAILURE, &error);
    DPI_CHECK_PTR_NOT_NULL(obj, prevIndex)
    DPI_CHECK_PTR_NOT_NULL(obj, exists)
    status = dpiOci__tablePrev(obj, index, prevIndex, exists, &error);
    return dpiGen__endPublicFn(obj, status, &error);
}


//-----------------------------------------------------------------------------
// dpiObject_getSize() [PUBLIC]
//   Return the size of the collection.
//-----------------------------------------------------------------------------
int dpiObject_getSize(dpiObject *obj, int32_t *size)
{
    dpiError error;
    int status;

    if (dpiObject__checkIsCollection(obj, __func__, &error) < 0)
        return dpiGen__endPublicFn(obj, DPI_FAILURE, &error);
    DPI_CHECK_PTR_NOT_NULL(obj, size)
    status = dpiOci__collSize(obj->type->conn, obj->instance, size, &error);
    return dpiGen__endPublicFn(obj, status, &error);
}


//-----------------------------------------------------------------------------
// dpiObject_release() [PUBLIC]
//   Release a reference to the object.
//-----------------------------------------------------------------------------
int dpiObject_release(dpiObject *obj)
{
    return dpiGen__release(obj, DPI_HTYPE_OBJECT, __func__);
}


//-----------------------------------------------------------------------------
// dpiObject_setAttributeValue() [PUBLIC]
//   Create a copy of the object and return it. Return NULL upon error.
//-----------------------------------------------------------------------------
int dpiObject_setAttributeValue(dpiObject *obj, dpiObjectAttr *attr,
        dpiNativeTypeNum nativeTypeNum, dpiData *data)
{
    void *valueIndicator, *ociValue;
    dpiOracleDataBuffer valueBuffer;
    int16_t scalarValueIndicator;
    dpiLob *lob = NULL;
    dpiError error;
    int status;

    // validate parameters
    if (dpiObject__check(obj, __func__, &error) < 0)
        return DPI_FAILURE;
    DPI_CHECK_PTR_NOT_NULL(obj, data)
    if (dpiGen__checkHandle(attr, DPI_HTYPE_OBJECT_ATTR, "set attribute value",
            &error) < 0)
        return dpiGen__endPublicFn(obj, DPI_FAILURE, &error);
    if (attr->belongsToType->tdo != obj->type->tdo) {
        dpiError__set(&error, "set attribute value", DPI_ERR_WRONG_ATTR,
                attr->nameLength, attr->name, obj->type->schemaLength,
                obj->type->schema, obj->type->nameLength, obj->type->name);
        return dpiGen__endPublicFn(obj, DPI_FAILURE, &error);
    }

    // check to see if type is supported
    if (!attr->typeInfo.oracleTypeNum) {
        dpiError__set(&error, "get attribute value",
                DPI_ERR_UNHANDLED_DATA_TYPE, attr->typeInfo.ociTypeCode);
        return dpiGen__endPublicFn(obj, DPI_FAILURE, &error);
    }

    // convert to input data format
    status = dpiObject__toOracleValue(obj, &error, &attr->typeInfo,
            &valueBuffer, &lob, &ociValue, &scalarValueIndicator,
            &valueIndicator, nativeTypeNum, data);

    // set attribute value
    if (status == DPI_SUCCESS)
        status = dpiOci__objectSetAttr(obj, attr, scalarValueIndicator,
                valueIndicator, ociValue, &error);
    dpiObject__clearOracleValue(obj, &error, &valueBuffer, lob,
            attr->typeInfo.oracleTypeNum);
    return dpiGen__endPublicFn(obj, status, &error);
}


//-----------------------------------------------------------------------------
// dpiObject_setElementValueByIndex() [PUBLIC]
//   Set the element at the specified index to the given value.
//-----------------------------------------------------------------------------
int dpiObject_setElementValueByIndex(dpiObject *obj, int32_t index,
        dpiNativeTypeNum nativeTypeNum, dpiData *data)
{
    dpiOracleDataBuffer valueBuffer;
    int16_t scalarValueIndicator;
    dpiLob *lob = NULL;
    void *indicator;
    dpiError error;
    void *ociValue;
    int status;

    if (dpiObject__checkIsCollection(obj, __func__, &error) < 0)
        return dpiGen__endPublicFn(obj, DPI_FAILURE, &error);
    DPI_CHECK_PTR_NOT_NULL(obj, data)
    status = dpiObject__toOracleValue(obj, &error, &obj->type->elementTypeInfo,
            &valueBuffer, &lob, &ociValue, &scalarValueIndicator,
            (void**) &indicator, nativeTypeNum, data);
    if (status == DPI_SUCCESS) {
        if (!indicator)
            indicator = &scalarValueIndicator;
        status = dpiOci__collAssignElem(obj->type->conn, index, ociValue,
                indicator, obj->instance, &error);
    }
    dpiObject__clearOracleValue(obj, &error, &valueBuffer, lob,
            obj->type->elementTypeInfo.oracleTypeNum);
    return dpiGen__endPublicFn(obj, status, &error);
}


//-----------------------------------------------------------------------------
// dpiObject_trim() [PUBLIC]
//   Trim a number of elements from the end of the collection.
//-----------------------------------------------------------------------------
int dpiObject_trim(dpiObject *obj, uint32_t numToTrim)
{
    dpiError error;
    int status;

    if (dpiObject__checkIsCollection(obj, __func__, &error) < 0)
        return dpiGen__endPublicFn(obj, DPI_FAILURE, &error);
    status = dpiOci__collTrim(obj->type->conn, numToTrim, obj->instance,
            &error);
    return dpiGen__endPublicFn(obj, status, &error);
}
