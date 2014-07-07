/** This file is part of Contacts daemon
 **
 ** Copyright (c) 2014 Jolla Ltd.
 **
 ** Contact: Matt Vogt <matthew.vogt@jollamobile.com>
 **
 ** GNU Lesser General Public License Usage
 ** This file may be used under the terms of the GNU Lesser General Public License
 ** version 2.1 as published by the Free Software Foundation and appearing in the
 ** file LICENSE.LGPL included in the packaging of this file.  Please review the
 ** following information to ensure the GNU Lesser General Public License version
 ** 2.1 requirements will be met:
 ** http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
 **/

#include "cdexportercontroller.h"
#include "cdexporterplugin.h"
#include "debug.h"

#include <qtcontacts-extensions_impl.h>
#include <qtcontacts-extensions_manager_impl.h>

#include <contactmanagerengine.h>
#include <twowaycontactsyncadapter.h>
#include <twowaycontactsyncadapter_impl.h>
#include <qcontactoriginmetadata_impl.h>

#include <QContactChangeLogFilter>
#include <QContactGuid>
#include <QContactIdFilter>

#include <QDateTime>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCall>

#include <unistd.h>

// buteo-syncfw
#include <ProfileEngineDefs.h>
#include <SyncProfile.h>
#include <SyncCommonDefs.h>
#include <ProfileManager.h>

using namespace Contactsd;

namespace {

const QString exportSyncTarget(QStringLiteral("export"));
const QString aggregateSyncTarget(QStringLiteral("aggregate"));
const QString oobIdsKey(QStringLiteral("privilegedIds"));
const QString avatarPathsKey(QStringLiteral("avatarPaths"));

// Delay 500ms for accumulate futher changes when a contact is updated
// Wait 10s for further changes when a contact presence is updated
const int syncDelay = 500;
const int presenceSyncDelay = 10000;

QMap<QString, QString> privilegedManagerParameters()
{
    QMap<QString, QString> rv;
    rv.insert(QStringLiteral("mergePresenceChanges"), QStringLiteral("false"));
    return rv;
}

QMap<QString, QString> nonprivilegedManagerParameters()
{
    QMap<QString, QString> rv;
    rv.insert(QStringLiteral("mergePresenceChanges"), QStringLiteral("false"));
    rv.insert(QStringLiteral("nonprivileged"), QStringLiteral("true"));
    return rv;
}

QString managerName()
{
    return QStringLiteral("org.nemomobile.contacts.sqlite");
}

QContactDetailFilter syncTargetFilter(const QString &syncTarget)
{
    QContactDetailFilter filter;
    filter.setDetailType(QContactSyncTarget::Type, QContactSyncTarget::FieldSyncTarget);
    filter.setValue(syncTarget);
    return filter;
}

QContactFilter modifiedSinceFilter(const QDateTime &ts)
{
    // Return local contacts modified since the timestamp
    QContactChangeLogFilter addedFilter, changedFilter;

    addedFilter.setEventType(QContactChangeLogFilter::EventAdded);
    addedFilter.setSince(ts);

    changedFilter.setEventType(QContactChangeLogFilter::EventChanged);
    changedFilter.setSince(ts);

    return syncTargetFilter(aggregateSyncTarget) & (addedFilter | changedFilter);
}

QContactFilter removedSinceFilter(const QDateTime &ts)
{
    // Return local contacts removed since the timestamp
    QContactChangeLogFilter removedFilter;

    removedFilter.setEventType(QContactChangeLogFilter::EventRemoved);
    removedFilter.setSince(ts);

    return syncTargetFilter(aggregateSyncTarget) & removedFilter;
}

QString mangleDetailUri(const QString &uri, const QContactId &privilegedId)
{
    if (uri.startsWith(aggregateSyncTarget)) {
        int index = uri.indexOf(QChar::fromLatin1(':'));
        if (index != -1) {
            // The URI may be of the obsolete form "'aggregate'-<ID>:" - if so, remove the ID portion
            return aggregateSyncTarget + uri.mid(index);
        }
    }

    return uri;
}

void mangleDetailUris(QContact &contact, const QContactId &privilegedId)
{
    foreach (const QContactDetail &detail, contact.details()) {
        bool modified(false);

        QString detailUri(detail.detailUri());
        if (!detailUri.isEmpty()) {
            detailUri = mangleDetailUri(detailUri, privilegedId);
            modified = true;
        }

        QStringList linkedDetailUris(detail.linkedDetailUris());
        if (!linkedDetailUris.isEmpty()) {
            QStringList::iterator it = linkedDetailUris.begin(), end = linkedDetailUris.end();
            for ( ; it != end; ++it) {
                QString &linkedUri(*it);
                if (!linkedUri.isEmpty()) {
                    linkedUri = mangleDetailUri(linkedUri, privilegedId);
                    modified = true;
                }
            }
        }

        if (modified) {
            QContactDetail copy(detail);
            if (!detailUri.isEmpty()) {
                copy.setDetailUri(detailUri);
            }
            if (!linkedDetailUris.isEmpty()) {
                copy.setLinkedDetailUris(linkedDetailUris);
            }
            contact.saveDetail(&copy);
        }
    }
}

QString demangleDetailUri(const QString &uri)
{
    if (uri.startsWith(aggregateSyncTarget)) {
        int index = uri.indexOf(QChar::fromLatin1(':'));
        if (index != -1) {
            return uri.mid(index + 1);
        }
    }

    return uri;
}

void demangleDetailUris(QContact &contact)
{
    foreach (const QContactDetail &detail, contact.details()) {
        bool modified(false);

        QString detailUri(detail.detailUri());
        if (!detailUri.isEmpty()) {
            detailUri = demangleDetailUri(detailUri);
            modified = true;
        }

        QStringList linkedDetailUris(detail.linkedDetailUris());
        if (!linkedDetailUris.isEmpty()) {
            QStringList::iterator it = linkedDetailUris.begin(), end = linkedDetailUris.end();
            for ( ; it != end; ++it) {
                QString &linkedUri(*it);
                if (!linkedUri.isEmpty()) {
                    linkedUri = demangleDetailUri(linkedUri);
                    modified = true;
                }
            }
        }

        if (modified) {
            QContactDetail copy(detail);
            if (!detailUri.isEmpty()) {
                copy.setDetailUri(detailUri);
            }
            if (!linkedDetailUris.isEmpty()) {
                copy.setLinkedDetailUris(linkedDetailUris);
            }
            contact.saveDetail(&copy);
        }
    }
}

void removeProvenanceInformation(QContact &contact)
{
    foreach (const QContactDetail &detail, contact.details()) {
        if (detail.hasValue(QContactDetail__FieldProvenance)) {
            QContactDetail copy(detail);
            copy.removeValue(QContactDetail__FieldProvenance);
            contact.saveDetail(&copy);
        }
    }
}

QHash<QUrl, QUrl> modifyAvatarUrls(QContact &contact)
{
    QHash<QUrl, QUrl> changes;

    foreach (const QContactAvatar &avatar, contact.details<QContactAvatar>()) {
        const QUrl imageUrl(avatar.imageUrl());
        if (imageUrl.scheme().isEmpty() || imageUrl.isLocalFile()) {
            // Avatar paths may indicate files not accessible to non-privileged apps
            // Link to them from an accessible path, and update the stored path in the avatar detail
            const QString path(avatar.imageUrl().path());
            if (QFile::exists(path)) {
                const QFileInfo fi(path);
                const QString privilegedPath(fi.absoluteFilePath());

                int index = privilegedPath.indexOf(QStringLiteral("/privileged/Contacts/"));
                if (index != -1) {
                    // Try to make a nonprivileged version of this file
                    QString nonprivilegedPath(QString(privilegedPath).remove(index, 11));
                    if (!QFile::exists(nonprivilegedPath)) {
                        // Ensure the target directory exists
                        index = nonprivilegedPath.lastIndexOf(QLatin1Char('/'));
                        if (index) {
                            const QString dirPath(nonprivilegedPath.left(index));
                            QDir dir(dirPath);
                            if (!dir.exists() && !QDir::root().mkpath(dirPath)) {
                                qWarning() << "Unable to create directory path:" << dirPath;
                            }
                        }

                        // Try to link the file to the new path
                        QByteArray oldPath(privilegedPath.toUtf8());
                        QByteArray newPath(nonprivilegedPath.toUtf8());
                        if (::link(oldPath.constData(), newPath.constData()) != 0) {
                            qWarning() << "Unable to create link to:" << privilegedPath << nonprivilegedPath;
                            nonprivilegedPath = QString();
                        }
                    }

                    // Update the avatar to point to the alternative path
                    QContactAvatar copy(avatar);
                    copy.setImageUrl(QUrl::fromLocalFile(nonprivilegedPath));
                    contact.saveDetail(&copy);

                    changes.insert(copy.imageUrl(), avatar.imageUrl());
                }
            }
        }
    }

    return changes;
}

void reverseAvatarChanges(QContact &contact, const QHash<QUrl, QUrl> &changes)
{
    foreach (const QContactAvatar &avatar, contact.details<QContactAvatar>()) {
        const QHash<QUrl, QUrl>::const_iterator it = changes.constFind(avatar.imageUrl());
        if (it != changes.constEnd()) {
            // This avatar's URL is one we changed it to; revert the change to prevent
            // it being detected as a nonprivileged modification
            QContactAvatar copy(avatar);
            copy.setImageUrl(*it);
            contact.saveDetail(&copy);
        }
    }
}

QSet<QContactDetail::DetailType> getIgnorableDetailTypes()
{
    QSet<QContactDetail::DetailType> rv;
    rv.insert(QContactDetail__TypeDeactivated);
    rv.insert(QContactDetail::TypeDisplayLabel);
    rv.insert(QContactDetail::TypeGlobalPresence);
    rv.insert(QContactDetail__TypeIncidental);
    rv.insert(QContactDetail__TypeStatusFlags);
    rv.insert(QContactDetail::TypeSyncTarget);
    rv.insert(QContactDetail::TypeTimestamp);
    return rv;
}

const QSet<QContactDetail::DetailType> &ignorableDetailTypes()
{
    static QSet<QContactDetail::DetailType> types(getIgnorableDetailTypes());
    return types;
}

QSet<QContactDetail::DetailType> getPresenceDetailTypes()
{
    QSet<QContactDetail::DetailType> rv;
    rv.insert(QContactDetail::TypePresence);
    rv.insert(QContactDetail::TypeOnlineAccount);
    rv.insert(QContactDetail__TypeOriginMetadata);
    return rv;
}

bool presenceOnlyChange(const QContact &oldContact, const QContact &newContact)
{
    // A presence-only change affects only { Presence, OnlineAccount, OriginMetadata }
    static QSet<QContactDetail::DetailType> presenceTypes(getPresenceDetailTypes());

    QList<QContactDetail> oldDetails(oldContact.details());

    QList<QContactDetail>::const_iterator it = oldDetails.constBegin(), end = oldDetails.constEnd();
    for ( ; it != end; ++it) {
        QContactDetail::DetailType type((*it).type());
        if (presenceTypes.contains(type) || ignorableDetailTypes().contains(type)) {
            continue;
        }

        QList<QContactDetail>::const_iterator pit = oldDetails.constBegin();
        while (pit != it && (*pit).type() != type) {
            ++pit;
        }
        if (pit != it) {
            // We have already tested this detail type
            continue;
        }

        // Test details of this type for changes
        QList<QContactDetail> oldTypeDetails(oldContact.details(type));
        QList<QContactDetail> newTypeDetails(newContact.details(type));
        if (newTypeDetails.count() != oldTypeDetails.count()) {
            return false;
        }

        // Compare the values to see if there are changes
        QList<QContactDetail>::const_iterator oit = oldTypeDetails.constBegin(), oend = oldTypeDetails.constEnd();
        for ( ; oit != oend; ++oit) {
            QContactDetail oldDetail(*oit);

            // Ignore differences in detail URIs
            oldDetail.removeValue(QContactDetail::FieldDetailUri);
            oldDetail.removeValue(QContactDetail::FieldLinkedDetailUris);

            QList<QContactDetail>::iterator nit = newTypeDetails.begin(), nend = newTypeDetails.end();
            for ( ; nit != nend; ++nit) {
                QContactDetail newDetail(*nit);
                newDetail.removeValue(QContactDetail::FieldDetailUri);
                newDetail.removeValue(QContactDetail::FieldLinkedDetailUris);
                if (newDetail == oldDetail) {
                    break;
                }
            }
            if (nit != nend) {
                // Don't match any other details to this one
                newTypeDetails.erase(nit);
            } else {
                // No match found - this difference prevents a presence-only change
                return false;
            }
        }
    }

    return true;
}

class SyncAdapter : public QtContactsSqliteExtensions::TwoWayContactSyncAdapter
{
private:
    QString m_accountId;
    QContactManager &m_privileged;
    QContactManager &m_nonprivileged;
    QDateTime m_remoteSince;
    QMap<QString, QString> m_privilegedIds;     // Hold in string form; conversion to QContactId is expensive and likely unnecessary
    QMap<QString, QString> m_nonprivilegedIds;  // As above.
    QContactId m_privilegedSelfId;
    QContactId m_nonprivilegedSelfId;
    bool m_privilegedIdsModified;
    QHash<QContactId, QHash<QUrl, QUrl> > m_avatarPathChanges;
    bool m_avatarPathChangesModified;

    QContactId getPrivilegedId(const QContactId &nonprivilegedId) const { return QContactId::fromString(m_privilegedIds.value(nonprivilegedId.toString())); }
    QContactId getNonprivilegedId(const QContactId &privilegedId) const { return QContactId::fromString(m_nonprivilegedIds.value(privilegedId.toString())); }

    void registerIdPair(const QString &privilegedId, const QString &nonprivilegedId)
    {
        m_privilegedIds.insert(nonprivilegedId, privilegedId);
        m_nonprivilegedIds.insert(privilegedId, nonprivilegedId);
        m_privilegedIdsModified = true;
    }
    void deregisterIdPair(const QString &privilegedId, const QString &nonprivilegedId)
    {
        QMap<QString, QString>::iterator pit = m_privilegedIds.find(nonprivilegedId);
        if (pit != m_privilegedIds.end()) {
            if (*pit != privilegedId) {
                qWarning() << "Mismatch on ID pair deregistration:" << *pit << "!=" << privilegedId;
                m_privilegedIds.erase(pit);
            }
        }
        QMap<QString, QString>::iterator nit = m_nonprivilegedIds.find(privilegedId);
        if (nit != m_nonprivilegedIds.end()) {
            if (*nit != nonprivilegedId) {
                qWarning() << "Mismatch on ID pair deregistration:" << *nit << "!=" << nonprivilegedId;
                m_nonprivilegedIds.erase(nit);
            }
        }
        m_privilegedIdsModified = true;
    }

    void registerAvatarPathChange(const QContactId &contactId, const QHash<QUrl, QUrl> &changes)
    {
        m_avatarPathChanges.insert(contactId, changes);
        m_avatarPathChangesModified = true;
    }
    void deregisterAvatarPathChange(const QContactId &contactId)
    {
        m_avatarPathChanges.remove(contactId);
        m_avatarPathChangesModified = true;
    }

    bool prepareSync()
    {
        if (!initSyncAdapter(m_accountId)) {
            qWarning() << "Unable to initialize sync adapter";
            return false;
        }

        if (!readSyncStateData(&m_remoteSince, m_accountId, TwoWayContactSyncAdapter::ReadPartialState)) {
            qWarning() << "Unable to read sync state data";
            return false;
        }

        // Read our extra OOB data
        QMap<QString, QVariant> values;
        if (!d->m_engine->fetchOOB(d->m_stateData[m_accountId].m_oobScope, QStringList() << oobIdsKey << avatarPathsKey, &values)) {
            qWarning() << "Failed to read sync state data for" << exportSyncTarget;
            return false;
        }

        // Read the IDs from storage
        {
            QByteArray cdata = values.value(oobIdsKey).toByteArray();
            QDataStream ds(cdata);
            ds >> m_privilegedIds;
        }

        // Create a reversed ID mapping
        QMap<QString, QString>::const_iterator it = m_privilegedIds.constBegin(), end = m_privilegedIds.constEnd();
        for ( ; it != end; ++it) {
            m_nonprivilegedIds.insert(it.value(), it.key());
        }

        // Ensure that the self IDS are mapped to each other
        if (m_privilegedIds.isEmpty()) {
            const QString privilegedSelfId(m_privilegedSelfId.toString());
            const QString nonprivilegedSelfId(m_nonprivilegedSelfId.toString());

            registerIdPair(privilegedSelfId, nonprivilegedSelfId);
        }

        // Retrieve any avatar path changes we have made
        {
            QByteArray cdata = values.value(avatarPathsKey).toByteArray();
            QDataStream ds(cdata);
            ds >> m_avatarPathChanges;
        }

        return true;
    }

    bool finalizeSync()
    {
        // Store the set of existing IDs to OOB
        QMap<QString, QVariant> values;

        if (m_privilegedIdsModified) {
            QByteArray cdata;
            {
                QDataStream write(&cdata, QIODevice::WriteOnly);
                write << m_privilegedIds;
            }
            values.insert(oobIdsKey, QVariant(cdata));
        }

        if (m_avatarPathChangesModified) {
            QByteArray cdata;
            {
                QDataStream write(&cdata, QIODevice::WriteOnly);
                write << m_avatarPathChanges;
            }
            values.insert(avatarPathsKey, QVariant(cdata));
        }

        if (!values.isEmpty()) {
            if (!d->m_engine->storeOOB(d->m_stateData[m_accountId].m_oobScope, values)) {
                qWarning() << "Failed to store sync state data to OOB storage";
                return false;
            }
        }

        if (!storeSyncStateData(m_accountId)) {
            qWarning() << "Unable to store final state after sync completion";
            return false;
        }

        return true;
    }

    void prepareImportContact(QContact &contact, const QContactId &privilegedId)
    {
        // Remove the timestamp detail from this contact
        QContactTimestamp ts(contact.detail<QContactTimestamp>());
        contact.removeDetail(&ts);

        // Remap avatar path changes
        if (!privilegedId.isNull()) {
            // If we modified this contact's avatar path, reverse that change
            QHash<QContactId, QHash<QUrl, QUrl> >::const_iterator it = m_avatarPathChanges.constFind(privilegedId);
            if (it != m_avatarPathChanges.constEnd()) {
                reverseAvatarChanges(contact, *it);
            }
        }

        // Mangle detail URIs to match the privileged DB data
        mangleDetailUris(contact, privilegedId);

        removeProvenanceInformation(contact);
    }

    bool syncNonprivilegedToPrivileged(bool importChanges, bool debug)
    {
        QList<QContact> modifiedContacts;
        QList<QContact> removedContacts;
        QContact selfContact;

        QList<QPair<QContactId, int> > additionIds;

        if (importChanges) {
            // Find nonprivileged changes since our last sync
            QList<QContactId> modifiedIds = m_nonprivileged.contactIds(modifiedSinceFilter(m_remoteSince));
            QList<QContactId> removedIds = m_nonprivileged.contactIds(removedSinceFilter(m_remoteSince));

            if (!modifiedIds.isEmpty() || !removedIds.isEmpty()) {
                QContactFetchHint fetchHint;
                fetchHint.setOptimizationHints(QContactFetchHint::NoRelationships);

                foreach (QContact contact, m_nonprivileged.contacts(modifiedIds, fetchHint)) {
                    // Find the primary DB ID for this contact, if it exists there
                    const QContactId nonprivilegedId(contact.id());

                    // Self contact must be treated separately
                    if (nonprivilegedId == m_nonprivilegedSelfId) {
                        selfContact = contact;
                        selfContact.setId(m_privilegedSelfId);

                        prepareImportContact(selfContact, m_privilegedSelfId);
                        continue;
                    }

                    const QContactId privilegedId(getPrivilegedId(nonprivilegedId));
                    contact.setId(privilegedId);
                    if (privilegedId.isNull()) {
                        // This is an addition
                        additionIds.append(qMakePair(nonprivilegedId, modifiedContacts.count()));
                    }

                    // Reset the syncTarget to 'aggregate' for this contact
                    QContactSyncTarget st(contact.detail<QContactSyncTarget>());
                    st.setSyncTarget(aggregateSyncTarget);
                    contact.saveDetail(&st);

                    // Remove any GUID from this contact
                    QContactGuid guid(contact.detail<QContactGuid>());
                    contact.removeDetail(&guid);

                    prepareImportContact(contact, privilegedId);

                    modifiedContacts.append(contact);
                }

                foreach (const QContactId &nonprivilegedId, removedIds) {
                    const QContactId privilegedId(getPrivilegedId(nonprivilegedId));
                    if (!privilegedId.isNull()) {
                        QContact contact;
                        contact.setId(privilegedId);
                        removedContacts.append(contact);
                    } else {
                        qWarning() << "Cannot remove export deletion without primary ID:" << nonprivilegedId;
                    }
                }
            }
        }

        if (debug) {
            qDebug() << "remote changes ================================";
            qDebug() << "m_remoteSince:" << m_remoteSince;
            qDebug() << "removedContacts:" << removedContacts;
            qDebug() << "modifiedContacts:" << modifiedContacts;
        } else {
            if (!removedContacts.isEmpty() || !modifiedContacts.isEmpty()) {
                qWarning() << "CDExport: importing changes:" << removedContacts.count() << (modifiedContacts.count() - additionIds.count()) << additionIds.count();
            }
        }

        if (!storeRemoteChanges(removedContacts, &modifiedContacts, m_accountId)) {
            qWarning() << "Unable to store remote changes";
            return false;
        }

        if (!additionIds.isEmpty()) {
            // Find the IDs allocated in the primary DB
            QList<QPair<QContactId, int> >::const_iterator it = additionIds.constBegin(), end = additionIds.constEnd();
            for ( ; it != end; ++it) {
                const QString nonprivilegedId((*it).first.toString());
                const int additionIndex((*it).second);
                const QString privilegedId(modifiedContacts.at(additionIndex).id().toString());

                registerIdPair(privilegedId, nonprivilegedId);
            }
        }

        if (!selfContact.id().isNull()) {
            // Store the self contact changes separately, with the original sync target
            removedContacts.clear();
            modifiedContacts.clear();
            modifiedContacts.append(selfContact);
            if (!storeRemoteChanges(removedContacts, &modifiedContacts, m_accountId)) {
                qWarning() << "Unable to store remote changes to self contact";
                return false;
            }
        }

        return true;
    }

    void prepareExportContact(QContact &contact, const QContactId &privilegedId)
    {
        // Remove the timestamp detail
        QContactTimestamp ts(contact.detail<QContactTimestamp>());
        contact.removeDetail(&ts);

        // Remap avatar path changes
        registerAvatarPathChange(privilegedId, modifyAvatarUrls(contact));

        // Remove any detail URI mangling used in the privileged DB
        demangleDetailUris(contact);

        removeProvenanceInformation(contact);
    }

    bool syncPrivilegedToNonprivileged(bool importChanges, bool debug)
    {
        // Find privileged DB changes we need to reflect (including presence changes)
        QDateTime localSince;
        QList<QContact> locallyAdded, locallyModified, locallyDeleted;
        if (!determineLocalChanges(&localSince, &locallyAdded, &locallyModified, &locallyDeleted, m_accountId, ignorableDetailTypes())) {
            qWarning() << "Unable to determine local changes";
            return false;
        }

        if (debug) {
            qDebug() << "local changes --------------------------------";
            qDebug() << "localSince:" << localSince;
            qDebug() << "locallyAdded:" << locallyAdded;
            qDebug() << "locallyModified:" << locallyModified;
            qDebug() << "locallyDeleted:" << locallyDeleted;
        } else {
            if (!locallyAdded.isEmpty() || !locallyModified.isEmpty() || !locallyDeleted.isEmpty()) {
                qWarning() << "CDExport: exporting changes:" << locallyAdded.count() << locallyModified.count() << locallyDeleted.count();
            }
        }

        QList<QContact> addedContacts;
        QList<QContact> modifiedContacts;
        QList<QContactId> removedContactIds;
        QContact selfContact;

        QList<QContactId> additionIds;

        // Apply primary DB changes to the nonprivileged DB
        foreach (QContact contact, locallyDeleted) {
            const QContactId privilegedId(contact.id());
            const QContactId nonprivilegedId(getNonprivilegedId(privilegedId));
            if (!nonprivilegedId.isNull()) {
                removedContactIds.append(nonprivilegedId);
            }

            deregisterAvatarPathChange(privilegedId);
        }

        // Note: a contact reported as deleted cannot also be in the added or modified lists
        foreach (QContact contact, locallyAdded + locallyModified) {
            const QContactId privilegedId(contact.id());

            if (privilegedId == m_privilegedSelfId) {
                selfContact = contact;
                selfContact.setId(m_nonprivilegedSelfId);

                prepareExportContact(selfContact, m_privilegedSelfId);
                continue;
            }

            const QContactId nonprivilegedId(getNonprivilegedId(privilegedId));
            contact.setId(nonprivilegedId);
            if (nonprivilegedId.isNull()) {
                // This is an addition
                additionIds.append(privilegedId);

                // Remove the primary DB ID
                contact.setId(QContactId());
            }

            // Represent this contact as an aggregate in the export DB
            QContactSyncTarget st = contact.detail<QContactSyncTarget>();
            st.setSyncTarget(aggregateSyncTarget);
            contact.saveDetail(&st);

            prepareExportContact(contact, privilegedId);

            if (nonprivilegedId.isNull()) {
                addedContacts.append(contact);
            } else {
                modifiedContacts.append(contact);
            }
        }

        // Remove any deleted contacts first so their details cannot conflict with subsequent additions
        while (!removedContactIds.isEmpty()) {
            QMap<int, QContactManager::Error> removeErrors;
            if (m_nonprivileged.removeContacts(removedContactIds, &removeErrors)) {
                break;
            } else {
                if (!removeErrors.isEmpty()) {
                    // Failures due to local non-existence do not concern us
                    QMap<int, QContactManager::Error>::const_iterator it = removeErrors.constBegin(), end = removeErrors.constEnd();
                    for ( ; it != end; ++it) {
                        if (it.value() != QContactManager::DoesNotExistError) {
                            // This error is a problem we shouldn't ignore
                            qWarning() << "Error removing ID:" << removedContactIds.at(it.key()) << "error:" << it.value();
                            break;
                        }
                    }
                    if (it == end) {
                        // All errors are inconsequential - remove the offending IDs and try again
                        QList<int> removeIndices(removeErrors.keys());
                        while (!removeIndices.isEmpty()) {
                            removedContactIds.removeAt(removeIndices.takeLast());
                        }
                        continue;
                    }
                }

                qWarning() << "Unable to remove privileged DB deletions from export DB!";

                // Removing contacts is less important than updating - if we can perform updates,
                // then failure to remove should not abort the sync attempt
                if (!modifiedContacts.isEmpty()) {
                    break;
                }
                return false;
            }
        }

        if (!modifiedContacts.isEmpty() || !addedContacts.isEmpty()) {
            if (!modifiedContacts.isEmpty()) {
                // Partition this modified set into two groups - those that only include presence changes, and the remainder
                QMap<QContactId, const QContact *> potentialPresenceChanges;
                foreach (const QContact &contact, modifiedContacts) {
                    // Only contacts with online accounts can have presence changes
                    if (!contact.details<QContactOnlineAccount>().isEmpty()) {
                        potentialPresenceChanges.insert(contact.id(), &contact);
                    }
                }

                if (!potentialPresenceChanges.isEmpty()) {
                    QList<QContact> presenceChangedContacts;

                    QContactIdFilter idFilter;
                    idFilter.setIds(potentialPresenceChanges.keys());
                    QContactFetchHint fetchHint;
                    fetchHint.setOptimizationHints(QContactFetchHint::NoRelationships);

                    QSet<QContactId> presenceChangedIds;
                    foreach (QContact contact, m_nonprivileged.contacts(idFilter, QList<QContactSortOrder>(), fetchHint)) {
                        const QContact *newContact(potentialPresenceChanges.value(contact.id()));
                        if (presenceOnlyChange(contact, *newContact)) {
                            presenceChangedContacts.append(*newContact);
                            presenceChangedIds.insert(contact.id());
                        }
                    }

                    if (!presenceChangedIds.isEmpty()) {
                        // Remove the presence-only changed contacts from the modified list
                        QList<QContact>::iterator it = modifiedContacts.begin();
                        while (it != modifiedContacts.end()) {
                            if (presenceChangedIds.contains((*it).id())) {
                                it = modifiedContacts.erase(it);
                            } else {
                                ++it;
                            }
                        }
                    }

                    // Save the presence changes first
                    if (!m_nonprivileged.saveContacts(&presenceChangedContacts, getPresenceDetailTypes().toList())) {
                        qWarning() << "Unable to save privileged DB presence changes to export DB!";
                        // Don't abort the sync operation for presence update failure
                    }
                }
            }

            size_t addedContactsOffset = modifiedContacts.count();
            if (!addedContacts.isEmpty()) {
                modifiedContacts.append(addedContacts);
            }

            while (!modifiedContacts.isEmpty()) {
                QMap<int, QContactManager::Error> saveErrors;
                if (m_nonprivileged.saveContacts(&modifiedContacts, &saveErrors)) {
                    break;
                }

                if (!importChanges) {
                    // If changes to the export database are not reimported, then deletions from the
                    // export database must be handled by recreating the contact
                    if (!saveErrors.isEmpty()) {
                        // Are these errors that we can handle?
                        QMap<int, QContactManager::Error>::const_iterator it = saveErrors.constBegin(), end = saveErrors.constEnd();
                        for ( ; it != end; ++it) {
                            // If some contact in the batch does not exist, any others in the batch will report LockedError
                            if ((it.value() != QContactManager::DoesNotExistError) &&
                                (it.value() != QContactManager::LockedError)) {
                                // This error is a problem we shouldn't ignore
                                qWarning() << "Error updating ID:" << modifiedContacts.at(it.key()).id() << "error:" << it.value();
                                break;
                            }
                        }
                        if (it == end) {
                            // All errors can be handled - convert the failed modifications to additions
                            QList<int> removeIndices;
                            for (it = saveErrors.constBegin(); it != end; ++it) {
                                if (it.value() == QContactManager::DoesNotExistError) {
                                    const int index(it.key());
                                    QContact modified(modifiedContacts.at(index));

                                    const QContactId obsoleteId(modified.id());
                                    const QContactId privilegedId(getPrivilegedId(obsoleteId));

                                    deregisterIdPair(privilegedId.toString(), obsoleteId.toString());

                                    removeIndices.append(index);

                                    // Convert the failed modification to an addition
                                    modified.setId(QContactId());
                                    modifiedContacts.append(modified);
                                    additionIds.append(privilegedId);
                                    qWarning() << "Recreating remotely deleted contact:" << privilegedId;
                                }
                            }

                            // Remove the invalid modifications from the save list
                            while (!removeIndices.isEmpty()) {
                                const int index(removeIndices.takeLast());
                                modifiedContacts.removeAt(index);
                                --addedContactsOffset;
                            }

                            // Attempt the save again
                            continue;
                        }
                    }
                }

                // We can't handle this error - abort the save
                qWarning() << "Unable to save privileged DB modifications to export DB!";
                return false;
            }

            if (!additionIds.isEmpty()) {
                // Find the IDs allocated in the export DB
                QList<QContactId>::const_iterator iit = additionIds.constBegin(), iend = additionIds.constEnd();
                QList<QContact>::const_iterator cit = modifiedContacts.constBegin() + addedContactsOffset;
                for ( ; iit != iend; ++iit, ++cit) {
                    const QString privilegedId((*iit).toString());
                    const QString nonprivilegedId((*cit).id().toString());

                    registerIdPair(privilegedId, nonprivilegedId);
                }
            }
        }

        if (!selfContact.id().isNull()) {
            if (!m_nonprivileged.saveContact(&selfContact)) {
                // Do not abort the sync attempt for this error
                qWarning() << "Unable to save privileged DB self contact changes to export DB!";
            }
        }

        return true;
    }

public:
    SyncAdapter(QContactManager &privileged, QContactManager &nonprivileged)
        : TwoWayContactSyncAdapter(exportSyncTarget, privileged)
        , m_privileged(privileged)
        , m_nonprivileged(nonprivileged)
        , m_privilegedIdsModified(false)
        , m_avatarPathChangesModified(false)
    {
        m_privilegedSelfId = m_privileged.selfContactId();
        m_nonprivilegedSelfId = m_nonprivileged.selfContactId();
    }

    bool sync(bool importChanges, bool debug)
    {
        // Proceed through the steps of the TWCSA algorithm, where the nonprivileged database
        // is equivalent to 'remote' and the privileged database is equivalent to 'local'
        return (prepareSync() &&
                syncNonprivilegedToPrivileged(importChanges, debug) &&
                syncPrivilegedToNonprivileged(importChanges, debug) &&
                finalizeSync());
    }
};

}

CDExporterController::CDExporterController(QObject *parent)
    : QObject(parent)
    , m_privilegedManager(managerName(), privilegedManagerParameters())
    , m_nonprivilegedManager(managerName(), nonprivilegedManagerParameters())
    , m_disabledConf(QStringLiteral("/org/nemomobile/contacts/export/disabled"))
    , m_debugConf(QStringLiteral("/org/nemomobile/contacts/export/debug"))
    , m_importConf(QStringLiteral("/org/nemomobile/contacts/export/import"))
{
    // Use a timer to delay reaction, so we don't sync until sequential changes have completed
    m_syncTimer.setSingleShot(true);
    connect(&m_syncTimer, SIGNAL(timeout()), this, SLOT(onSyncTimeout()));

    connect(&m_privilegedManager, SIGNAL(contactsAdded(QList<QContactId>)), this, SLOT(onPrivilegedContactsAdded(QList<QContactId>)));
    connect(&m_privilegedManager, SIGNAL(contactsChanged(QList<QContactId>)), this, SLOT(onPrivilegedContactsChanged(QList<QContactId>)));
    connect(&m_privilegedManager, SIGNAL(contactsRemoved(QList<QContactId>)), this, SLOT(onPrivilegedContactsRemoved(QList<QContactId>)));

    // Presence changes are reported by the engine
    QtContactsSqliteExtensions::ContactManagerEngine *engine(QtContactsSqliteExtensions::contactManagerEngine(m_privilegedManager));
    connect(engine, SIGNAL(contactsPresenceChanged(QList<QContactId>)), this, SLOT(onPrivilegedContactsPresenceChanged(QList<QContactId>)));
    connect(engine, SIGNAL(syncContactsChanged(QStringList)), this, SLOT(onSyncContactsChanged(QStringList)));

    connect(&m_nonprivilegedManager, SIGNAL(contactsAdded(QList<QContactId>)), this, SLOT(onNonprivilegedContactsAdded(QList<QContactId>)));
    connect(&m_nonprivilegedManager, SIGNAL(contactsChanged(QList<QContactId>)), this, SLOT(onNonprivilegedContactsChanged(QList<QContactId>)));
    connect(&m_nonprivilegedManager, SIGNAL(contactsRemoved(QList<QContactId>)), this, SLOT(onNonprivilegedContactsRemoved(QList<QContactId>)));

    if (m_disabledConf.value().toInt() == 0) {
        // Do an initial sync
        m_syncTimer.start(1);
    } else {
        qWarning() << "Contacts database export is disabled";
    }
}

CDExporterController::~CDExporterController()
{
}

void CDExporterController::onPrivilegedContactsAdded(const QList<QContactId> &addedIds)
{
    Q_UNUSED(addedIds)
    scheduleSync(DataChange);
}

void CDExporterController::onPrivilegedContactsChanged(const QList<QContactId> &changedIds)
{
    Q_UNUSED(changedIds)
    scheduleSync(DataChange);
}

void CDExporterController::onPrivilegedContactsPresenceChanged(const QList<QContactId> &changedIds)
{
    Q_UNUSED(changedIds)
    scheduleSync(PresenceChange);
}

void CDExporterController::onPrivilegedContactsRemoved(const QList<QContactId> &removedIds)
{
    Q_UNUSED(removedIds)
    scheduleSync(DataChange);
}

void CDExporterController::onNonprivilegedContactsAdded(const QList<QContactId> &addedIds)
{
    Q_UNUSED(addedIds)
    if (m_importConf.value().toInt() > 0) {
        scheduleSync(DataChange);
    }
}

void CDExporterController::onNonprivilegedContactsChanged(const QList<QContactId> &changedIds)
{
    Q_UNUSED(changedIds)
    if (m_importConf.value().toInt() > 0) {
        scheduleSync(DataChange);
    }
}

void CDExporterController::onNonprivilegedContactsRemoved(const QList<QContactId> &removedIds)
{
    Q_UNUSED(removedIds)
    if (m_importConf.value().toInt() > 0) {
        scheduleSync(DataChange);
    }
}

void CDExporterController::onSyncContactsChanged(const QStringList &syncTargets)
{
    Q_FOREACH (const QString &syncTarget, syncTargets) {
        m_syncTargetsNeedingSync.insert(syncTarget);
    }
}


void CDExporterController::onSyncTimeout()
{
    const bool importChanges(m_importConf.value().toInt() > 0);
    const bool debug(m_debugConf.value().toInt() > 0);

    // Perform a sync between the privileged and non-privileged managers
    SyncAdapter adapter(m_privilegedManager, m_nonprivilegedManager);
    if (!adapter.sync(importChanges, debug)) {
        qWarning() << "Unable to synchronize database changes!";
    }

    // And trigger a sync to external Contacts sync sources
    if (!m_syncTargetsNeedingSync.isEmpty()) {
        qWarning() << "CDExport: triggering contacts sync" << QStringList(m_syncTargetsNeedingSync.toList()).join(QStringLiteral(":"));
        QDBusMessage message = QDBusMessage::createMethodCall(
                QStringLiteral("com.nokia.contactsd"),
                QStringLiteral("/SyncTrigger"),
                QStringLiteral("com.nokia.contactsd"),
                QStringLiteral("triggerSync"));
        message.setArguments(QVariantList()
                << QVariant::fromValue<QStringList>(QStringList(m_syncTargetsNeedingSync.toList()))
                << QVariant::fromValue<qint32>(1)   // only if AlwaysUpToDate set in profile
                << QVariant::fromValue<qint32>(1)); // only if Upsync or TwoWay direction
        QDBusConnection::sessionBus().asyncCall(message);
        m_syncTargetsNeedingSync.clear();
    }
}

void CDExporterController::scheduleSync(ChangeType type)
{
    // Something has changed that needs to be exported
    if (m_disabledConf.value().toInt() == 0) {
        m_syncTimer.start(type == PresenceChange ? presenceSyncDelay : syncDelay);
    }
}

