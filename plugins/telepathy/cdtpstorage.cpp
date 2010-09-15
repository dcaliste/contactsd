/***************************************************************************
**
** Copyright (C) 2010 Nokia Corporation and/or its subsidiary(-ies).
** All rights reserved.
** Contact: Nokia Corporation (people-users@projects.maemo.org)
**
** This file is part of contactsd.
**
** If you have questions regarding the use of this file, please contact
** Nokia at people-users@projects.maemo.org.
**
** This library is free software; you can redistribute it and/or
** modify it under the terms of the GNU Lesser General Public
** License version 2.1 as published by the Free Software Foundation
** and appearing in the file LICENSE.LGPL included in the packaging
** of this file.
**
****************************************************************************/

#include "cdtpstorage.h"

#include <TelepathyQt4/ContactCapabilities>

CDTpStorage::CDTpStorage(QObject *parent)
    : QObject(parent)
{
}

CDTpStorage::~CDTpStorage()
{
}

void CDTpStorage::syncAccount(CDTpAccount *accountWrapper)
{
    syncAccount(accountWrapper, CDTpAccount::All);
}

// TODO: Improve syncAccount so that it only updates the data that really
//       changed
void CDTpStorage::syncAccount(CDTpAccount *accountWrapper,
        CDTpAccount::Changes changes)
{
    Tp::AccountPtr account = accountWrapper->account();
    QString accountObjectPath = account->objectPath();
    const QString strLocalUID = QString::number(0x7FFFFFFF);

    qDebug() << "Syncing account" << accountObjectPath << "to storage";

    const QUrl accountUrl(QString("telepathy:%1").arg(accountObjectPath));
    QString paramAccount = account->parameters()["account"].toString();
    const QUrl imAddressUrl(QString("telepathy:%1!%2")
            .arg(accountObjectPath).arg(paramAccount));

    RDFUpdate up;

    RDFVariable imAccount(accountUrl);
    RDFVariable imAddress(imAddressUrl);

    up.addInsertion(imAccount, rdf::type::iri(), nco::IMAccount::iri());
    up.addInsertion(imAccount, nco::imAccountType::iri(), LiteralValue(account->protocol()));

    up.addInsertion(imAddress, rdf::type::iri(), nco::IMAddress::iri());
    up.addInsertion(imAddress, nco::imID::iri(), LiteralValue(paramAccount));

    if (changes & CDTpAccount::DisplayName) {
        up.addInsertion(imAccount, nco::imDisplayName::iri(), LiteralValue(account->displayName()));
    }

    if (changes & CDTpAccount::Nickname) {
       up.addInsertion(imAddress, nco::imNickname::iri(), LiteralValue(account->displayName()));
    }

    if (changes & CDTpAccount::Presence) {
        Tp::SimplePresence presence = account->currentPresence();

        up.addInsertion(imAddress, nco::imStatusMessage::iri(),
                LiteralValue(presence.statusMessage));
        up.addInsertion(imAddress, nco::imPresence::iri(),
                LiteralValue(trackerStatusFromTpPresenceStatus(presence.status)));
        up.addInsertion(imAddress, nco::presenceLastModified::iri(),
                LiteralValue(QDateTime::currentDateTime()));
    }

    // link the IMAddress to me-contact
    up.addInsertion(nco::default_contact_me::iri(), nco::contactLocalUID::iri(),
            LiteralValue(strLocalUID));
    up.addInsertion(nco::default_contact_me::iri(), nco::hasIMAddress::iri(), imAddress);
    up.addInsertion(imAccount, nco::imAccountAddress::iri(), imAddress);

    if (changes & CDTpAccount::Avatar) {
        QString fileName;
        const Tp::Avatar &avatar = account->avatar();
        // TODO: saving to disk needs to be removed here
        const bool ok = saveAccountAvatar(avatar.avatarData, avatar.MIMEType,
                QString("%1/.contacts/avatars/").arg(QDir::homePath()), fileName);
        updateAvatar(up, imAddressUrl, QUrl::fromLocalFile(fileName), ok);
    }

    ::tracker()->executeQuery(up);
}

void CDTpStorage::syncAccountContacts(CDTpAccount *accountWrapper)
{
    // TODO: return the number of contacts that were actually added
    syncAccountContacts(accountWrapper, accountWrapper->contacts(),
            QList<CDTpContact *>());
}

void CDTpStorage::syncAccountContacts(CDTpAccount *accountWrapper,
        const QList<CDTpContact *> &contactsAdded,
        const QList<CDTpContact *> &contactsRemoved)
{
    Tp::AccountPtr account = accountWrapper->account();
    QString accountObjectPath = account->objectPath();

    qDebug() << "Syncing account" << accountObjectPath <<
        "roster contacts to storage";
    qDebug() << " " << contactsAdded.size() << "contacts added";
    qDebug() << " " << contactsRemoved.size() << "contacts removed";

    RDFUpdate updateQuery;
    foreach (CDTpContact *contactWrapper, contactsAdded) {
        Tp::ContactPtr contact = contactWrapper->contact();

        const QString id = contact->id();
        const QString localId = contactLocalId(accountObjectPath, id);

        const RDFVariable imContact(contactIri(localId));
        const RDFVariable imAddress(contactImAddress(accountObjectPath, id));
        const RDFVariable imAccount(QUrl(QString("telepathy:%1").arg(accountObjectPath)));
        const QDateTime datetime = QDateTime::currentDateTime();

        updateQuery.addDeletion(imContact, nie::contentLastModified::iri());

        updateQuery.addInsertion(RDFStatementList() <<
                RDFStatement(imAddress, rdf::type::iri(), nco::IMAddress::iri()) <<
                RDFStatement(imAddress, nco::imID::iri(), LiteralValue(id)));

        updateQuery.addInsertion(RDFStatementList() <<
                RDFStatement(imContact, rdf::type::iri(), nco::PersonContact::iri()) <<
                RDFStatement(imContact, nco::hasIMAddress::iri(), imAddress) <<
                RDFStatement(imContact, nco::contactLocalUID::iri(), LiteralValue(localId)) <<
                RDFStatement(imContact, nco::contactUID::iri(), LiteralValue(localId)));

        updateQuery.addInsertion(RDFStatementList() <<
                RDFStatement(imAccount, rdf::type::iri(), nco::IMAccount::iri()) <<
                RDFStatement(imAccount, nco::hasIMContact::iri(), imAddress));

        updateQuery.addInsertion(imContact, nie::contentLastModified::iri(), RDFVariable(datetime));

        addContactAliasInfoToQuery(updateQuery, imAddress, contactWrapper);
        addContactPresenceInfoToQuery(updateQuery, imAddress, contactWrapper);
        addContactCapabilitiesInfoToQuery(updateQuery, imAddress, contactWrapper);
        addContactAvatarInfoToQuery(updateQuery, imAddress, contactWrapper);
    }

    foreach (CDTpContact *contactWrapper, contactsRemoved) {
        addContactAvatarInfoToQuery(updateQuery, accountWrapper, contactWrapper);
    }

    if (!contactsAdded.isEmpty()) {
        ::tracker()->executeQuery(updateQuery);
    }

    if (!contactsRemoved.isEmpty()) {
        ::tracker()->executeQuery(updateQuery);
    }
}

void CDTpStorage::syncAccountContact(CDTpAccount *accountWrapper,
        CDTpContact *contactWrapper, CDTpContact::Changes changes)
{
    Tp::AccountPtr account = accountWrapper->account();
    Tp::ContactPtr contact = contactWrapper->contact();

    const QString id = contact->id();
    const QString localId = contactLocalId(account->objectPath(), id);
    const RDFVariable imContact(contactIri(localId));

    qDebug() << "Syncing account" << account->objectPath() <<
        "contact" << contact->id() << "changes to storage";

    RDFUpdate updateQuery;
    const RDFVariable imAddress(contactImAddress(contactWrapper));

    updateQuery.addDeletion(imContact, nie::contentLastModified::iri());
    updateQuery.addInsertion(imContact, nie::contentLastModified::iri(),
            RDFVariable(QDateTime::currentDateTime()));

    if (changes & CDTpContact::Alias) {
        qDebug() << "  alias changed";
        addContactAliasInfoToQuery(updateQuery, imAddress, contactWrapper);
    }
    if (changes & CDTpContact::Presence) {
        qDebug() << "  presence changed";
        addContactPresenceInfoToQuery(updateQuery, imAddress, contactWrapper);
    }
    if (changes & CDTpContact::Capabilities) {
        qDebug() << "  capabilities changed";
        addContactCapabilitiesInfoToQuery(updateQuery, imAddress, contactWrapper);
    }
    if (changes & CDTpContact::Avatar) {
        qDebug() << "  avatar changed";
        addContactAvatarInfoToQuery(updateQuery, imAddress, contactWrapper);
    }

    ::tracker()->executeQuery(updateQuery);
}

void CDTpStorage::setAccountContactsOffline(CDTpAccount *accountWrapper)
{
    Tp::AccountPtr account = accountWrapper->account();

    qDebug() << "Setting account" << account->objectPath() <<
        "contacts presence to Offline on storage";

    RDFUpdate updateQuery;
    foreach (CDTpContact *contactWrapper, accountWrapper->contacts()) {
        const RDFVariable imAddress(contactImAddress(contactWrapper));

        updateQuery.addDeletion(imAddress, nco::imPresence::iri());
        updateQuery.addDeletion(imAddress, nco::imStatusMessage::iri());
        updateQuery.addDeletion(imAddress, nco::presenceLastModified::iri());

        const QLatin1String status("unknown");
        updateQuery.addInsertion(RDFStatementList() <<
                RDFStatement(imAddress, nco::imStatusMessage::iri(),
                    LiteralValue("")) <<
                RDFStatement(imAddress, nco::imPresence::iri(),
                    trackerStatusFromTpPresenceStatus(status)));

        updateQuery.addInsertion(imAddress, nco::presenceLastModified::iri(),
                RDFVariable(QDateTime::currentDateTime()));
    }
    ::tracker()->executeQuery(updateQuery);
}

void CDTpStorage::removeAccount(const QString &accountObjectPath)
{
    qDebug() << "Removing account" << accountObjectPath << "from storage";

    RDFVariable imContact = RDFVariable::fromType<nco::PersonContact>();
    RDFVariable imAddress = imContact.optional().property<nco::hasIMAddress>();
    RDFVariable imAccount = RDFVariable::fromType<nco::IMAccount>();
    imAccount.property<nco::hasIMContact>() = imAddress;
    imAccount = QUrl("telepathy:" + accountObjectPath);

    RDFSelect select;
    select.addColumn("contact", imContact);
    select.addColumn("distinct", imAddress.property<nco::imID>());
    select.addColumn("contactId", imContact.property<nco::contactLocalUID>());
    select.addColumn("accountPath", imAccount);
    select.addColumn("address", imAddress);

    CDTpStorageSelectQuery *query = new CDTpStorageSelectQuery(select, this);
    connect(query,
            SIGNAL(finished(CDTpStorageSelectQuery *)),
            SLOT(onAccountRemovalSelectQueryFinished(CDTpStorageSelectQuery *)));
}

void CDTpStorage::onAccountRemovalSelectQueryFinished(CDTpStorageSelectQuery *query)
{
    RDFUpdate update;

    LiveNodes removalNodes = query->reply();
    for (int i = 0; i < removalNodes->rowCount(); ++i) {
        const QString imTrackerAddress = removalNodes->index(i, 1).data().toString();
        const QString imTrackerLocalId = removalNodes->index(i, 2).data().toString();
        const QString imTrackerAccountPath = removalNodes->index(i, 3).data().toString();

        const QString accountObjectPath(imTrackerAccountPath.split(":").value(1));

        const RDFVariable imContact(contactIri(imTrackerLocalId));
        const RDFVariable imAddress(contactImAddress(accountObjectPath, imTrackerAddress));
        const RDFVariable imAccount(QUrl(QString("telepathy:%1").arg(accountObjectPath)));

        update.addDeletion(imContact, nco::PersonContact::iri());
        update.addDeletion(imAddress, nco::IMAddress::iri());
        update.addDeletion(imAccount, nco::IMAccount::iri());
    }

    ::tracker()->executeQuery(update);

    query->deleteLater();
}

bool CDTpStorage::saveAccountAvatar(const QByteArray &data, const QString &mimeType,
        const QString &path, QString &fileName)
{
    Q_UNUSED(mimeType);

    if (data.isEmpty()) {
        // nothing to write, avatar is empty
        return false;
    }

    fileName = path + QString(QCryptographicHash::hash(data,
                QCryptographicHash::Sha1).toHex());
    qDebug() << "Saving account avatar to" << fileName;

    QFile avatarFile(fileName);
    if (!avatarFile.open(QIODevice::WriteOnly)) {
        qWarning() << "Unable to save account avatar: error opening avatar "
            "file" << fileName << "for writing";
        return false;
    }
    avatarFile.write(data);
    avatarFile.close();

    qDebug() << "Account avatar saved successfully";

    return true;
}

void CDTpStorage::addContactAliasInfoToQuery(RDFUpdate &query,
        const RDFVariable &imAddress,
        CDTpContact *contactWrapper)
{
    Tp::ContactPtr contact = contactWrapper->contact();

    query.addDeletion(imAddress, nco::imNickname::iri());

    query.addInsertion(RDFStatement(imAddress, nco::imNickname::iri(),
                LiteralValue(contact->alias())));
}

void CDTpStorage::addContactPresenceInfoToQuery(RDFUpdate &query,
        const RDFVariable &imAddress,
        CDTpContact *contactWrapper)
{
    Tp::ContactPtr contact = contactWrapper->contact();

    query.addDeletion(imAddress, nco::imPresence::iri());
    query.addDeletion(imAddress, nco::imStatusMessage::iri());
    query.addDeletion(imAddress, nco::presenceLastModified::iri());

    query.addInsertion(RDFStatementList() <<
            RDFStatement(imAddress, nco::imStatusMessage::iri(),
                LiteralValue(contact->presenceMessage())) <<
            RDFStatement(imAddress, nco::imPresence::iri(),
                trackerStatusFromTpPresenceType(contact->presenceType())) << 
            RDFStatement(imAddress, nco::presenceLastModified::iri(),
                RDFVariable(QDateTime::currentDateTime())));
}

void CDTpStorage::addContactCapabilitiesInfoToQuery(RDFUpdate &query,
        const RDFVariable &imAddress,
        CDTpContact *contactWrapper)
{
    Tp::ContactPtr contact = contactWrapper->contact();

    query.addDeletion(imAddress, nco::imCapability::iri());

    if (contact->capabilities()->supportsTextChats()) {
        query.addInsertion(RDFStatementList() <<
                RDFStatement(imAddress, nco::imCapability::iri(),
                    nco::im_capability_text_chat::iri()));
    }

    if (contact->capabilities()->supportsAudioCalls()) {
        query.addInsertion(RDFStatementList() <<
                RDFStatement(imAddress, nco::imCapability::iri(),
                    nco::im_capability_audio_calls::iri()));
    }

    if (contact->capabilities()->supportsVideoCalls()) {
        query.addInsertion(RDFStatementList() <<
                RDFStatement(imAddress, nco::imCapability::iri(),
                    nco::im_capability_video_calls::iri()));
    }
}

void CDTpStorage::addContactAvatarInfoToQuery(RDFUpdate &query,
        const RDFVariable &imAddress,
        CDTpContact *contactWrapper)
{
    Tp::ContactPtr contact = contactWrapper->contact();

    /* If we don't know the avatar token, it is preferable to keep the old
     * avatar until we get an update. */
    if (!contact->isAvatarTokenKnown()) {
        return;
    }

    /* If we have a token but not an avatar filename, that probably means the
     * avatar data is being requested and we'll get an update later. */
    if (!contact->avatarToken().isEmpty() &&
        contact->avatarData().fileName.isEmpty()) {
        return;
    }

    RDFVariable dataObject(QUrl::fromLocalFile(contact->avatarData().fileName));

    query.addDeletion(imAddress, nco::imAvatar::iri());

    if (!contact->avatarToken().isEmpty()) {
        query.addInsertion(RDFStatement(imAddress, nco::imAvatar::iri(), dataObject));
    }
}

void CDTpStorage::addContactRemoveInfoToQuery(RDFUpdate &query,
        CDTpAccount *accountWrapper,
        CDTpContact *contactWrapper)
{
    Tp::ContactPtr contact = contactWrapper->contact();
    QString accountObjectPath = accountWrapper->account()->objectPath();
    const QString id = contact->id();
    const QString localId = contactLocalId(accountObjectPath, id);
    const RDFVariable imContact(contactIri(localId));
    const RDFVariable imAddress(contactImAddress(accountObjectPath, id));
    const RDFVariable imAccount(QUrl(QString("telepathy:%1").arg(accountObjectPath)));

    query.addDeletion(imContact, nco::PersonContact::iri());
    query.addDeletion(imAccount, nco::hasIMContact::iri(), imAddress);
    query.addDeletion(imAddress, nco::IMAddress::iri());
}

QString CDTpStorage::contactLocalId(const QString &contactAccountObjectPath,
        const QString &contactId) const
{
    return QString::number(qHash(QString("%1!%2")
                .arg(contactAccountObjectPath)
                .arg(contactId)));
}

QString CDTpStorage::contactLocalId(CDTpContact *contactWrapper) const
{
    CDTpAccount *accountWrapper = contactWrapper->accountWrapper();
    Tp::AccountPtr account = accountWrapper->account();
    Tp::ContactPtr contact = contactWrapper->contact();
    return contactLocalId(account->objectPath(), contact->id());
}

QUrl CDTpStorage::contactIri(const QString &contactLocalId) const
{
    return QUrl(QString("contact:%1").arg(contactLocalId));
}

QUrl CDTpStorage::contactIri(CDTpContact *contactWrapper) const
{
    return contactIri(contactLocalId(contactWrapper));
}

QUrl CDTpStorage::contactImAddress(const QString &contactAccountObjectPath,
        const QString &contactId) const
{
    return QUrl(QString("telepathy:%1!%2")
            .arg(contactAccountObjectPath)
            .arg(contactId));
}

QUrl CDTpStorage::contactImAddress(CDTpContact *contactWrapper) const
{
    CDTpAccount *accountWrapper = contactWrapper->accountWrapper();
    Tp::AccountPtr account = accountWrapper->account();
    Tp::ContactPtr contact = contactWrapper->contact();
    return contactImAddress(account->objectPath(), contact->id());
}

QUrl CDTpStorage::trackerStatusFromTpPresenceType(uint tpPresenceType) const
{
    switch (tpPresenceType) {
    case Tp::ConnectionPresenceTypeUnset:
        return nco::presence_status_unknown::iri();
    case Tp::ConnectionPresenceTypeOffline:
        return nco::presence_status_offline::iri();
    case Tp::ConnectionPresenceTypeAvailable:
        return nco::presence_status_available::iri();
    case Tp::ConnectionPresenceTypeAway:
        return nco::presence_status_away::iri();
    case Tp::ConnectionPresenceTypeExtendedAway:
        return nco::presence_status_extended_away::iri();
    case Tp::ConnectionPresenceTypeHidden:
        return nco::presence_status_hidden::iri();
    case Tp::ConnectionPresenceTypeBusy:
        return nco::presence_status_busy::iri();
    case Tp::ConnectionPresenceTypeUnknown:
        return nco::presence_status_unknown::iri();
    case Tp::ConnectionPresenceTypeError:
        return nco::presence_status_error::iri();
    default:
        qWarning() << "Unknown telepathy presence status" << tpPresenceType;
    }

    return nco::presence_status_error::iri();
}

QUrl CDTpStorage::trackerStatusFromTpPresenceStatus(
        const QString &tpPresenceStatus) const
{
    static QHash<QString, QUrl> mapping;
    if (mapping.isEmpty()) {
        mapping.insert("offline", nco::presence_status_offline::iri());
        mapping.insert("available", nco::presence_status_available::iri());
        mapping.insert("away", nco::presence_status_away::iri());
        mapping.insert("xa", nco::presence_status_extended_away::iri());
        mapping.insert("dnd", nco::presence_status_busy::iri());
        mapping.insert("busy", nco::presence_status_busy::iri());
        mapping.insert("hidden", nco::presence_status_hidden::iri());
        mapping.insert("unknown", nco::presence_status_unknown::iri());
    }

    QHash<QString, QUrl>::const_iterator i(mapping.constFind(tpPresenceStatus));
    if (i != mapping.end()) {
        return *i;
    }
    return nco::presence_status_error::iri();
}

void CDTpStorage::updateAvatar(RDFUpdate &query,
        const QUrl &url,
        const QUrl &fileName,
        bool deleteOnly)
{
    // We need deleteOnly to handle cases where the avatar image was removed from the account
    if (!fileName.isValid()) {
        return;
    }

    RDFVariable imAddress(url);
    RDFVariable dataObject(fileName);

    query.addDeletion(imAddress, nco::imAvatar::iri());

    if (!deleteOnly) {
        query.addInsertion(RDFStatement(imAddress, nco::imAvatar::iri(), dataObject));
    }
}


CDTpStorageSelectQuery::CDTpStorageSelectQuery(const RDFSelect &select,
        QObject *parent)
    : QObject(parent)
{
    mReply = ::tracker()->modelQuery(select);
    connect(mReply.model(),
            SIGNAL(modelUpdated()),
            SLOT(onModelUpdated()));
}

CDTpStorageSelectQuery::~CDTpStorageSelectQuery()
{
}

void CDTpStorageSelectQuery::onModelUpdated()
{
    emit finished(this);
}
