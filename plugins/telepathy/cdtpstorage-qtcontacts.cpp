/** This file is part of Contacts daemon
 **
 ** Copyright (c) 2010-2011 Nokia Corporation and/or its subsidiary(-ies).
 **
 ** Contact:  Nokia Corporation (info@qt.nokia.com)
 **
 ** GNU Lesser General Public License Usage
 ** This file may be used under the terms of the GNU Lesser General Public License
 ** version 2.1 as published by the Free Software Foundation and appearing in the
 ** file LICENSE.LGPL included in the packaging of this file.  Please review the
 ** following information to ensure the GNU Lesser General Public License version
 ** 2.1 requirements will be met:
 ** http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
 **
 ** In addition, as a special exception, Nokia gives you certain additional rights.
 ** These rights are described in the Nokia Qt LGPL Exception version 1.1, included
 ** in the file LGPL_EXCEPTION.txt in this package.
 **
 ** Other Usage
 ** Alternatively, this file may be used in accordance with the terms and
 ** conditions contained in a signed written agreement between you and Nokia.
 **/

#include <TelepathyQt/AvatarData>
#include <TelepathyQt/ContactCapabilities>
#include <TelepathyQt/ContactManager>
#include <TelepathyQt/ConnectionCapabilities>

#include <QContact>
#include <QContactManager>
#include <QContactDetail>
#include <QContactDetailFilter>
#include <QContactIntersectionFilter>

#include <QContactAddress>
#include <QContactAvatar>
#include <QContactBirthday>
#include <QContactEmailAddress>
#include <QContactGender>
#include <QContactName>
#include <QContactNickname>
#include <QContactNote>
#include <QContactOnlineAccount>
#include <QContactOrganization>
#include <QContactPhoneNumber>
#include <QContactPresence>
#include <QContactSyncTarget>
#include <QContactUrl>

#include <qtcontacts-tracker/phoneutils.h>

#include "cdtpstorage-qtcontacts.h"
#include "cdtpavatarupdate.h"
#include "debug.h"

using namespace Contactsd;

// Uncomment for masses of debug output:
//#define DEBUG_OVERLOAD

namespace {

template<int N>
const QString &sourceLocation(const char *f)
{
    static const QString tmpl(QString::fromLatin1("%2:%1").arg(N));
    static const QString loc(tmpl.arg(QString::fromLatin1(f)));
    return loc;
}

#define SRC_LOC sourceLocation<__LINE__>(__PRETTY_FUNCTION__)

QString asString(bool f)
{
    return QLatin1String(f ? "true" : "false");
}

QString asString(const Tp::ContactInfoField &field, int i)
{
    if (i >= field.fieldValue.count()) {
        return QLatin1String("");
    }

    return field.fieldValue[i];
}

QStringList asStringList(const Tp::ContactInfoField &field, int i)
{
    QStringList rv;

    while (i < field.fieldValue.count()) {
        rv.append(field.fieldValue[i]);
        ++i;
    }

    return rv;
}

QString asString(CDTpContact::Info::Capability c)
{
    switch (c) {
        case CDTpContact::Info::TextChats:
            return QLatin1String("TextChats");
        case CDTpContact::Info::StreamedMediaCalls:
            return QLatin1String("StreamedMediaCalls");
        case CDTpContact::Info::StreamedMediaAudioCalls:
            return QLatin1String("StreamedMediaAudioCalls");
        case CDTpContact::Info::StreamedMediaAudioVideoCalls:
            return QLatin1String("StreamedMediaAudioVideoCalls");
        case CDTpContact::Info::UpgradingStreamMediaCalls:
            return QLatin1String("UpgradingStreamMediaCalls");
        case CDTpContact::Info::FileTransfers:
            return QLatin1String("FileTransfers");
        case CDTpContact::Info::StreamTubes:
            return QLatin1String("StreamTubes");
        case CDTpContact::Info::DBusTubes:
            return QLatin1String("DBusTubes");
        default:
            break;
    }

    return QString();
}

}

class Q_CONTACTS_EXPORT QContactTpMetadata : public QContactDetail
{
public:
    Q_DECLARE_CUSTOM_CONTACT_DETAIL(QContactTpMetadata, "TpMetadata")
    Q_DECLARE_LATIN1_CONSTANT(FieldContactId, "ContactId");
    Q_DECLARE_LATIN1_CONSTANT(FieldAccountId, "AccountId");
    Q_DECLARE_LATIN1_CONSTANT(FieldAccountEnabled, "AccountEnabled");

    void setContactId(const QString &s) { setValue(FieldContactId, s); }
    QString contactId() const { return value(FieldContactId); }

    void setAccountId(const QString &s) { setValue(FieldAccountId, s); }
    QString accountId() const { return value(FieldAccountId); }

    void setAccountEnabled(bool b) { setValue(FieldAccountEnabled, asString(b)); }
    bool accountEnabled() const { return (value(FieldAccountEnabled) == asString(true)); }

    static QContactDetailFilter matchContactId(const QString &s)
    {
        QContactDetailFilter filter;
        filter.setDetailDefinitionName(QContactTpMetadata::DefinitionName, FieldContactId);
        filter.setValue(s);
        filter.setMatchFlags(QContactFilter::MatchExactly);
        return filter;
    }

    static QContactDetailFilter matchAccountId(const QString &s)
    {
        QContactDetailFilter filter;
        filter.setDetailDefinitionName(QContactTpMetadata::DefinitionName, FieldAccountId);
        filter.setValue(s);
        filter.setMatchFlags(QContactFilter::MatchExactly);
        return filter;
    }
};

Q_IMPLEMENT_CUSTOM_CONTACT_DETAIL(QContactTpMetadata, "TpMetadata");
Q_DEFINE_LATIN1_CONSTANT(QContactTpMetadata::FieldContactId, "ContactId");
Q_DEFINE_LATIN1_CONSTANT(QContactTpMetadata::FieldAccountId, "AccountId");
Q_DEFINE_LATIN1_CONSTANT(QContactTpMetadata::FieldAccountEnabled, "AccountEnabled");

namespace {

const int UPDATE_TIMEOUT = 150; // ms
const int UPDATE_THRESHOLD = 50; // contacts

const QLatin1String QContactOnlineAccount__FieldAccountPath("AccountPath");
const QLatin1String QContactOnlineAccount__FieldAccountIconPath("AccountIconPath");
const QLatin1String QContactOnlineAccount__FieldEnabled("Enabled");

const QLatin1String QContactPhoneNumber__FieldNormalizedNumber("NormalizedNumber");

QContactManager *createManager()
{
    debug() << SRC_LOC << QContactManager::availableManagers();

    QString envspec(QLatin1String(qgetenv("NEMO_CONTACT_MANAGER")));
    if (!envspec.isEmpty()) {
        debug() << "Using contact manager:" << envspec;
        return new QContactManager(envspec);
    }

    return new QContactManager;
}

QContactManager *manager()
{
    static QContactManager *manager = createManager();
    return manager;
}

QContact selfContact()
{
    QContactManager *mgr(manager());

    // Check that there is a self contact
    int selfId = mgr->selfContactId();
    if (!selfId) {
        debug() << "Creating self contact";
        QContact self;
        if (!mgr->saveContact(&self)) {
            warning() << "Unable to save empty contact as self contact - error:" << mgr->error();
            return QContact();
        } else {
            selfId = self.localId();
            if (!mgr->setSelfContactId(selfId)) {
                warning() << "Unable to set contact ID as self contact ID - error:" << mgr->error();
                return QContact();
            }
        }
    }

    // Retrieve the self contact
    return mgr->contact(selfId);
}

template<typename Debug>
Debug output(Debug &debug, const QContactDetail &detail)
{
    const QVariantMap &values(detail.variantValues());
    QVariantMap::const_iterator it = values.constBegin(), end = values.constEnd();
    for ( ; it != end; ++it) {
        debug << "\n   -" << it.key() << ":" << it.value();
    }
    return debug;
}

template<typename Debug>
Debug output(Debug &debug, const QContact &contact)
{
    const QList<QContactDetail> &details(contact.details());
    foreach (const QContactDetail &detail, details) {
        debug << "\n  Detail:" << detail.definitionName();
        output(debug, detail);
    }
    return debug;
}

bool storeContactDetail(QContact &contact, QContactDetail &detail, const QString &location)
{
#ifdef DEBUG_OVERLOAD
    debug() << "  Storing" << detail.definitionName() << "from:" << location;
    output(debug(), detail);
    return contact.saveDetail(&detail);
#else
    if (!contact.saveDetail(&detail)) {
        debug() << "  Failed storing" << detail.definitionName() << "from:" << location;
        output(debug(), detail);
        return false;
    }
    return true;
#endif
}

QStringList contactChangesList(CDTpContact::Changes changes)
{
    QStringList rv;

    if (changes & CDTpContact::Alias) {
        rv.append(QContactNickname::DefinitionName);
    }
    if (changes & CDTpContact::Presence) {
        rv.append(QContactPresence::DefinitionName);
    }
    if (changes & CDTpContact::Capabilities) {
        rv.append(QContactOnlineAccount::DefinitionName);
    }
    if (changes & CDTpContact::Avatar) {
        rv.append(QContactAvatar::DefinitionName);
    }

    return rv;
}

bool storeContact(QContact &contact, const QString &location, CDTpContact::Changes changes = CDTpContact::All)
{
    QList<QContact> contacts;
    QStringList updates;

    const bool minimizedUpdate((changes != CDTpContact::All) && ((changes & CDTpContact::Information) == 0));
    if (minimizedUpdate) {
        contacts << contact;
        updates = contactChangesList(changes);
    }

#ifdef DEBUG_OVERLOAD
    debug() << "Storing contact" << contact.localId() << "from:" << location;
    output(debug(), contact);
    if (minimizedUpdate) {
        debug() << "Updating:" << updates;
        return manager()->saveContacts(&contacts, contactChangesList(changes));
    } else {
        return manager()->saveContact(&contact);
    }
#else
    if (minimizedUpdate) {
        if (!manager()->saveContacts(&contacts, contactChangesList(changes))) {
            debug() << "Failed minimized storing contact" << contact.localId() << "from:" << location;
            output(debug(), contact);
            debug() << "Updates" << updates;
            return false;
        }
    } else {
        if (!manager()->saveContact(&contact)) {
            debug() << "Failed storing contact" << contact.localId() << "from:" << location;
            output(debug(), contact);
            return false;
        }
    }
#endif
    return true;
}

QContactDetailFilter matchTelepathyFilter()
{
    QContactDetailFilter filter;
    filter.setDetailDefinitionName(QContactSyncTarget::DefinitionName, QContactSyncTarget::FieldSyncTarget);
    filter.setValue(QLatin1String("telepathy"));
    filter.setMatchFlags(QContactFilter::MatchExactly);
    return filter;
}

QList<QContactLocalId> findContactIdsForAccount(const QString &accountPath)
{
    QContactIntersectionFilter filter;
    filter << QContactTpMetadata::matchAccountId(accountPath);
    filter << matchTelepathyFilter();
    return manager()->contactIds(filter);
}

QContact findExistingContact(const QString &contactAddress)
{
    QContactIntersectionFilter filter;
    filter << QContactTpMetadata::matchContactId(contactAddress);
    filter << matchTelepathyFilter();

    foreach (const QContact &contact, manager()->contacts(filter)) {
        // Return the first match we find (there should be only one)
        return contact;
    }

    return QContact();
}

template<typename T>
T findLinkedDetail(const QContact &owner, const QContactDetail &link)
{
    const QString linkUri(link.detailUri());

    foreach (const T &detail, owner.details<T>()) {
        if (detail.linkedDetailUris().contains(linkUri)) {
            return detail;
        }
    }

    return T();
}

QContactPresence findPresenceForAccount(const QContact &owner, const QContactOnlineAccount &qcoa)
{
    return findLinkedDetail<QContactPresence>(owner, qcoa);
}

QContactAvatar findAvatarForAccount(const QContact &owner, const QContactOnlineAccount &qcoa)
{
    return findLinkedDetail<QContactAvatar>(owner, qcoa);
}

QString imAccount(Tp::AccountPtr account)
{
    return account->objectPath();
}

QString imAccount(CDTpAccountPtr accountWrapper)
{
    return imAccount(accountWrapper->account());
}

QString imAccount(CDTpContactPtr contactWrapper)
{
    return imAccount(contactWrapper->accountWrapper());
}

QString imAddress(const QString &accountPath, const QString &contactId = QString())
{
    static const QString tmpl = QString::fromLatin1("%1!%2");
    return tmpl.arg(accountPath, contactId.isEmpty() ? QLatin1String("self") : contactId);
}

QString imAddress(Tp::AccountPtr account, const QString &contactId = QString())
{
    return imAddress(imAccount(account), contactId);
}

QString imAddress(CDTpAccountPtr accountWrapper, const QString &contactId = QString())
{
    return imAddress(accountWrapper->account(), contactId);
}

QString imAddress(CDTpContactPtr contactWrapper)
{
    return imAddress(contactWrapper->accountWrapper(), contactWrapper->contact()->id());
}

QString imPresence(const QString &accountPath, const QString &contactId = QString())
{
    static const QString tmpl = QString::fromLatin1("%1!%2!presence");
    return tmpl.arg(accountPath, contactId.isEmpty() ? QLatin1String("self") : contactId);
}

QString imPresence(Tp::AccountPtr account, const QString &contactId = QString())
{
    return imPresence(imAccount(account), contactId);
}

QString imPresence(CDTpAccountPtr accountWrapper, const QString &contactId = QString())
{
    return imPresence(accountWrapper->account(), contactId);
}

QString imPresence(CDTpContactPtr contactWrapper)
{
    return imPresence(contactWrapper->accountWrapper(), contactWrapper->contact()->id());
}

QContactPresence::PresenceState qContactPresenceState(Tp::ConnectionPresenceType presenceType)
{
    switch (presenceType) {
    case Tp::ConnectionPresenceTypeOffline:
        return QContactPresence::PresenceOffline;

    case Tp::ConnectionPresenceTypeAvailable:
        return QContactPresence::PresenceAvailable;

    case Tp::ConnectionPresenceTypeAway:
        return QContactPresence::PresenceAway;

    case Tp::ConnectionPresenceTypeExtendedAway:
        return QContactPresence::PresenceExtendedAway;

    case Tp::ConnectionPresenceTypeHidden:
        return QContactPresence::PresenceHidden;

    case Tp::ConnectionPresenceTypeBusy:
        return QContactPresence::PresenceBusy;

    case Tp::ConnectionPresenceTypeUnknown:
    case Tp::ConnectionPresenceTypeUnset:
    case Tp::ConnectionPresenceTypeError:
        break;

    default:
        warning() << "Unknown telepathy presence status" << presenceType;
        break;
    }

    return QContactPresence::PresenceUnknown;
}

bool isOnlinePresence(Tp::ConnectionPresenceType presenceType, Tp::AccountPtr account)
{
    switch (presenceType) {
    // Why??
    case Tp::ConnectionPresenceTypeOffline:
        return account->protocolName() == QLatin1String("skype");

    case Tp::ConnectionPresenceTypeUnset:
    case Tp::ConnectionPresenceTypeUnknown:
    case Tp::ConnectionPresenceTypeError:
        return false;

    default:
        break;
    }

    return true;
}

QStringList currentCapabilites(const Tp::CapabilitiesBase &capabilities, Tp::ConnectionPresenceType presenceType, Tp::AccountPtr account)
{
    QStringList current;

    if (capabilities.textChats()) {
        current << asString(CDTpContact::Info::TextChats);
    }

    if (isOnlinePresence(presenceType, account)) {
        if (capabilities.streamedMediaCalls()) {
            current << asString(CDTpContact::Info::StreamedMediaCalls);
        }
        if (capabilities.streamedMediaAudioCalls()) {
            current << asString(CDTpContact::Info::StreamedMediaAudioCalls);
        }
        if (capabilities.streamedMediaVideoCalls()) {
            current << asString(CDTpContact::Info::StreamedMediaAudioVideoCalls);
        }
        if (capabilities.upgradingStreamedMediaCalls()) {
            current << asString(CDTpContact::Info::UpgradingStreamMediaCalls);
        }
        if (capabilities.fileTransfers()) {
            current << asString(CDTpContact::Info::FileTransfers);
        }
    }

    return current;
}

void updateContactAvatars(QContact &contact, const QString &defaultAvatarPath, const QString &largeAvatarPath, const QContactOnlineAccount &qcoa)
{
    static const QLatin1String contextLarge("Large");
    static const QLatin1String contextDefault("Default");

    QContactAvatar defaultAvatar;
    QContactAvatar largeAvatar;

    foreach (const QContactAvatar &detail, contact.details<QContactAvatar>()) {
        const QStringList &contexts(detail.contexts());
        if (contexts.contains(contextDefault)) {
            defaultAvatar = detail;
        } else if (contexts.contains(contextLarge)) {
            largeAvatar = detail;
        }
    }

    if (defaultAvatarPath.isEmpty()) {
        if (!defaultAvatar.isEmpty()) {
            if (!contact.removeDetail(&defaultAvatar)) {
                warning() << SRC_LOC << "Unable to remove default avatar from contact:" << contact.id();
            }
        }
    } else {
        defaultAvatar.setImageUrl(QUrl::fromLocalFile(defaultAvatarPath));
        defaultAvatar.setContexts(contextDefault);
        defaultAvatar.setLinkedDetailUris(qcoa.detailUri());
        if (!storeContactDetail(contact, defaultAvatar, SRC_LOC)) {
            warning() << SRC_LOC << "Unable to save default avatar for contact:" << contact.id();
        }
    }

    if (largeAvatarPath.isEmpty()) {
        if (!largeAvatar.isEmpty()) {
            if (!contact.removeDetail(&largeAvatar)) {
                warning() << SRC_LOC << "Unable to remove large avatar from contact:" << contact.id();
            }
        }
    } else {
        largeAvatar.setImageUrl(QUrl::fromLocalFile(largeAvatarPath));
        largeAvatar.setContexts(contextLarge);
        largeAvatar.setLinkedDetailUris(qcoa.detailUri());
        if (!storeContactDetail(contact, largeAvatar, SRC_LOC)) {
            warning() << SRC_LOC << "Unable to save large avatar for contact:" << contact.id();
        }
    }
}

QString saveAccountAvatar(CDTpAccountPtr accountWrapper)
{
    const Tp::Avatar &avatar = accountWrapper->account()->avatar();

    if (avatar.avatarData.isEmpty()) {
        return QString();
    }

    static const QString tmpl = QString::fromLatin1("%1/.contacts/avatars/%2");
    QString fileName = tmpl.arg(QDir::homePath())
        .arg(QLatin1String(QCryptographicHash::hash(avatar.avatarData, QCryptographicHash::Sha1).toHex()));

    QFile avatarFile(fileName);
    if (!avatarFile.open(QIODevice::WriteOnly)) {
        warning() << "Unable to save account avatar: error opening avatar file" << fileName << "for writing";
        return QString();
    }
    avatarFile.write(avatar.avatarData);
    avatarFile.close();

    return fileName;
}

void updateFacebookAvatar(QNetworkAccessManager &network, CDTpContactPtr contactWrapper, const QString &facebookId, const QString &avatarType)
{
    const QUrl avatarUrl(QLatin1String("http://graph.facebook.com/") % facebookId %
                         QLatin1String("/picture?type=") % avatarType);

    // CDTpAvatarUpdate keeps a weak reference to CDTpContact, since the contact is
    // also its parent. If we'd pass a CDTpContactPtr to the update, it'd keep a ref that
    // keeps the CDTpContact alive. Then, if the update is the last object to hold
    // a ref to the contact, the refcount of the contact will go to 0 when the update
    // dtor is called (for example from deleteLater). At this point, the update will
    // already be being deleted, but the dtor of CDTpContact will try to delete the
    // update a second time, causing a double free.
    QObject *const update = new CDTpAvatarUpdate(network.get(QNetworkRequest(avatarUrl)),
                                                 contactWrapper.data(),
                                                 avatarType,
                                                 contactWrapper.data());

    QObject::connect(update, SIGNAL(finished()), update, SLOT(deleteLater()));
}

void updateSocialAvatars(QNetworkAccessManager &network, CDTpContactPtr contactWrapper)
{
    if (network.networkAccessible() == QNetworkAccessManager::NotAccessible) {
        return;
    }

    QRegExp facebookIdPattern(QLatin1String("-(\\d+)@chat\\.facebook\\.com"));

    if (not facebookIdPattern.exactMatch(contactWrapper->contact()->id())) {
        return; // only supporting Facebook avatars right now
    }

    const QString socialId = facebookIdPattern.cap(1);

    updateFacebookAvatar(network, contactWrapper, socialId, CDTpAvatarUpdate::Large);
    updateFacebookAvatar(network, contactWrapper, socialId, CDTpAvatarUpdate::Square);
}

CDTpContact::Changes updateAccountDetails(QContact &self, QContactOnlineAccount &qcoa, QContactPresence &presence, CDTpAccountPtr accountWrapper, CDTpAccount::Changes changes)
{
    CDTpContact::Changes selfChanges = 0;

    const QString accountPath(imAccount(accountWrapper));
    debug() << "Update account" << accountPath;

    Tp::AccountPtr account = accountWrapper->account();

    if (changes & CDTpAccount::Presence) {
        Tp::Presence tpPresence(account->currentPresence());

        presence.setPresenceState(qContactPresenceState(tpPresence.type()));
        presence.setTimestamp(QDateTime::currentDateTime());
        presence.setCustomMessage(tpPresence.statusMessage());

        selfChanges |= CDTpContact::Presence;
    }
    if ((changes & CDTpAccount::Nickname) ||
        (changes & CDTpAccount::DisplayName)) {
        const QString displayName(account->displayName());
        const QString nickname(account->nickname());

        if (displayName.isEmpty()) {
            presence.setNickname(displayName);
        } else if (nickname.isEmpty()) {
            presence.setNickname(nickname);
        } else {
            presence.setNickname(QString());
        }

        selfChanges |= CDTpContact::Presence;
    }
    if (changes & CDTpAccount::Avatar) {
        static const QLatin1String contextDefault("Default");
        const QString avatarPath(saveAccountAvatar(accountWrapper));

        QContactAvatar avatar(findAvatarForAccount(self, qcoa));
        avatar.setLinkedDetailUris(qcoa.detailUri());

        if (avatarPath.isEmpty()) {
            if (!avatar.isEmpty()) {
                if (!self.removeDetail(&avatar)) {
                    warning() << SRC_LOC << "Unable to remove avatar for account:" << accountPath;
                }
            }
        } else {
            avatar.setImageUrl(QUrl::fromLocalFile(avatarPath));
            avatar.setContexts(contextDefault);

            if (!storeContactDetail(self, avatar, SRC_LOC)) {
                warning() << SRC_LOC << "Unable to save avatar for account:" << accountPath;
            }
        }

        selfChanges |= CDTpContact::Avatar;
    }

    if (selfChanges & CDTpContact::Capabilities) {
        // The account has changed
        if (!storeContactDetail(self, qcoa, SRC_LOC)) {
            warning() << SRC_LOC << "Unable to save details for self account:" << accountPath;
        }
    }

    if (selfChanges & CDTpContact::Presence) {
        if (!storeContactDetail(self, presence, SRC_LOC)) {
            warning() << SRC_LOC << "Unable to save presence for self account:" << accountPath;
        }
    }

    return selfChanges;
}

template<typename DetailType>
void deleteContactDetails(QContact &existingContact)
{
    foreach (DetailType detail, existingContact.details<DetailType>()) {
        if (!existingContact.removeDetail(&detail)) {
            warning() << SRC_LOC << "Unable to remove obsolete detail:" << detail.detailUri();
        }
    }
}

typedef QHash<QString, QString> Dictionary;

Dictionary initPhoneTypes()
{
    Dictionary types;

    types.insert(QLatin1String("bbsl"), QContactPhoneNumber::SubTypeBulletinBoardSystem);
    types.insert(QLatin1String("car"), QContactPhoneNumber::SubTypeCar);
    types.insert(QLatin1String("cell"), QContactPhoneNumber::SubTypeMobile);
    types.insert(QLatin1String("fax"), QContactPhoneNumber::SubTypeFax);
    types.insert(QLatin1String("modem"), QContactPhoneNumber::SubTypeModem);
    types.insert(QLatin1String("pager"), QContactPhoneNumber::SubTypePager);
    types.insert(QLatin1String("video"), QContactPhoneNumber::SubTypeVideo);
    types.insert(QLatin1String("voice"), QContactPhoneNumber::SubTypeVoice);
    // Not sure about these types:
    types.insert(QLatin1String("isdn"), QContactPhoneNumber::SubTypeLandline);
    types.insert(QLatin1String("pcs"), QContactPhoneNumber::SubTypeLandline);

    return types;
}

const Dictionary &phoneTypes()
{
    static Dictionary types(initPhoneTypes());
    return types;
}

Dictionary initAddressTypes()
{
    Dictionary types;

    types.insert(QLatin1String("dom"), QContactAddress::SubTypeDomestic);
    types.insert(QLatin1String("intl"), QContactAddress::SubTypeInternational);
    types.insert(QLatin1String("parcel"), QContactAddress::SubTypeParcel);
    types.insert(QLatin1String("postal"), QContactAddress::SubTypePostal);

    return types;
}

const Dictionary &addressTypes()
{
    static Dictionary types(initAddressTypes());
    return types;
}

Dictionary initGenderTypes()
{
    Dictionary types;

    types.insert(QLatin1String("f"), QContactGender::GenderFemale);
    types.insert(QLatin1String("female"), QContactGender::GenderFemale);
    types.insert(QLatin1String("m"), QContactGender::GenderMale);
    types.insert(QLatin1String("male"), QContactGender::GenderMale);

    return types;
}

const Dictionary &genderTypes()
{
    static Dictionary types(initGenderTypes());
    return types;
}

void updateContactDetails(QNetworkAccessManager &network, QContact &existingContact, CDTpContactPtr contactWrapper, CDTpContact::Changes changes)
{
    const QString contactAddress(imAddress(contactWrapper));
    debug() << "Update contact" << contactAddress;

    Tp::ContactPtr contact = contactWrapper->contact();

    // Apply changes
    if (changes & CDTpContact::Alias) {
        QContactNickname nickname = existingContact.detail<QContactNickname>();
        nickname.setNickname(contact->alias().trimmed());

        if (!storeContactDetail(existingContact, nickname, SRC_LOC)) {
            warning() << SRC_LOC << "Unable to save alias to contact for:" << contactAddress;
        }

        // The alias is also reflected in the presence
        changes |= CDTpContact::Presence;
    }
    if (changes & CDTpContact::Presence) {
        Tp::Presence tpPresence(contact->presence());

        QContactPresence presence = existingContact.detail<QContactPresence>();
        presence.setPresenceState(qContactPresenceState(tpPresence.type()));
        presence.setTimestamp(QDateTime::currentDateTime());
        presence.setCustomMessage(tpPresence.statusMessage());
        presence.setNickname(contact->alias().trimmed());

        if (!storeContactDetail(existingContact, presence, SRC_LOC)) {
            warning() << SRC_LOC << "Unable to save presence to contact for:" << contactAddress;
        }

        // Since we use static account capabilities as fallback, each presence also implies
        // a capability change. This doesn't fit the pure school of Telepathy, but we really
        // should not drop the static caps fallback at this stage.
        changes |= CDTpContact::Capabilities;
    }
    if (changes & CDTpContact::Capabilities) {
        QContactOnlineAccount qcoa = existingContact.detail<QContactOnlineAccount>();
        qcoa.setCapabilities(currentCapabilites(contact->capabilities(), contact->presence().type(), contactWrapper->accountWrapper()->account()));

        if (!storeContactDetail(existingContact, qcoa, SRC_LOC)) {
            warning() << SRC_LOC << "Unable to save capabilities to contact for:" << contactAddress;
        }
    }
    if (changes & CDTpContact::Information) {
        if (contactWrapper->isInformationKnown()) {
            // Delete any existing info we have for this contact
            deleteContactDetails<QContactAddress>(existingContact);
            deleteContactDetails<QContactBirthday>(existingContact);
            deleteContactDetails<QContactEmailAddress>(existingContact);
            deleteContactDetails<QContactGender>(existingContact);
            deleteContactDetails<QContactName>(existingContact);
            deleteContactDetails<QContactNickname>(existingContact);
            deleteContactDetails<QContactNote>(existingContact);
            deleteContactDetails<QContactOrganization>(existingContact);
            deleteContactDetails<QContactPhoneNumber>(existingContact);
            deleteContactDetails<QContactUrl>(existingContact);

            Tp::ContactInfoFieldList listContactInfo = contact->infoFields().allFields();
            if (listContactInfo.count() != 0) {
                const QLatin1String defaultContext("Other");
                const QLatin1String homeContext("Home");
                const QLatin1String workContext("Work");

                QContactOrganization organizationDetail;
                QContactName nameDetail;

                // Add any information reported by telepathy
                foreach (const Tp::ContactInfoField &field, listContactInfo) {
                    if (field.fieldValue.count() == 0) {
                        continue;
                    }

                    // Extract field types
                    QStringList subTypes;
                    QString detailContext;
                    foreach (const QString &param, field.parameters) {
                        if (!param.startsWith(QLatin1String("type="))) {
                            continue;
                        }
                        const QString type = param.mid(5);
                        if (type == QLatin1String("home")) {
                            detailContext = homeContext;
                        } else if (type == QLatin1String("work")) {
                            detailContext = workContext;
                        } else if (!subTypes.contains(type)){
                            subTypes << type;
                        }
                    }

                    if (field.fieldName == QLatin1String("tel")) {
                        QStringList selectedTypes;
                        foreach (const QString &type, subTypes) {
                            Dictionary::const_iterator it = phoneTypes().find(type.toLower());
                            if (it != phoneTypes().constEnd()) {
                                selectedTypes.append(*it);
                            }
                        }
                        if (selectedTypes.isEmpty()) {
                            // Assume landline
                            selectedTypes.append(QContactPhoneNumber::SubTypeLandline);
                        }

                        QContactPhoneNumber phoneNumberDetail;
                        phoneNumberDetail.setContexts(detailContext.isNull() ? defaultContext : detailContext);
                        phoneNumberDetail.setNumber(asString(field, 0));
                        phoneNumberDetail.setValue(QContactPhoneNumber__FieldNormalizedNumber, qctMakeLocalPhoneNumber(asString(field, 0)));
                        phoneNumberDetail.setSubTypes(subTypes);

                        if (!storeContactDetail(existingContact, phoneNumberDetail, SRC_LOC)) {
                            warning() << SRC_LOC << "Unable to save phone number to contact";
                        }
                    } else if (field.fieldName == QLatin1String("adr")) {
                        QStringList selectedTypes;
                        foreach (const QString &type, subTypes) {
                            Dictionary::const_iterator it = addressTypes().find(type.toLower());
                            if (it != addressTypes().constEnd()) {
                                selectedTypes.append(*it);
                            }
                        }

                        // QContactAddress does not support extended street address, so combine the fields
                        QString streetAddress(asString(field, 1) + QLatin1Char('\n') + asString(field, 2));

                        QContactAddress addressDetail;
                        if (!detailContext.isNull()) {
                            addressDetail.setContexts(detailContext);
                        }
                        if (selectedTypes.isEmpty()) {
                            addressDetail.setSubTypes(selectedTypes);
                        }
                        addressDetail.setPostOfficeBox(asString(field, 0));
                        addressDetail.setStreet(streetAddress);
                        addressDetail.setLocality(asString(field, 3));
                        addressDetail.setRegion(asString(field, 4));
                        addressDetail.setPostcode(asString(field, 5));
                        addressDetail.setCountry(asString(field, 6));

                        if (!storeContactDetail(existingContact, addressDetail, SRC_LOC)) {
                            warning() << SRC_LOC << "Unable to save address to contact";
                        }
                    } else if (field.fieldName == QLatin1String("email")) {
                        QContactEmailAddress emailDetail;
                        if (!detailContext.isNull()) {
                            emailDetail.setContexts(detailContext);
                        }
                        emailDetail.setEmailAddress(asString(field, 0));

                        if (!storeContactDetail(existingContact, emailDetail, SRC_LOC)) {
                            warning() << SRC_LOC << "Unable to save email address to contact";
                        }
                    } else if (field.fieldName == QLatin1String("url")) {
                        QContactUrl urlDetail;
                        if (!detailContext.isNull()) {
                            urlDetail.setContexts(detailContext);
                        }
                        urlDetail.setUrl(asString(field, 0));

                        if (!storeContactDetail(existingContact, urlDetail, SRC_LOC)) {
                            warning() << SRC_LOC << "Unable to save URL to contact";
                        }
                    } else if (field.fieldName == QLatin1String("title")) {
                        organizationDetail.setTitle(asString(field, 0));
                        if (!detailContext.isNull()) {
                            organizationDetail.setContexts(detailContext);
                        }
                    } else if (field.fieldName == QLatin1String("role")) {
                        organizationDetail.setRole(asString(field, 0));
                        if (!detailContext.isNull()) {
                            organizationDetail.setContexts(detailContext);
                        }
                    } else if (field.fieldName == QLatin1String("org")) {
                        organizationDetail.setName(asString(field, 0));
                        organizationDetail.setDepartment(asStringList(field, 1));
                        if (!detailContext.isNull()) {
                            organizationDetail.setContexts(detailContext);
                        }

                        if (!storeContactDetail(existingContact, organizationDetail, SRC_LOC)) {
                            warning() << SRC_LOC << "Unable to save organization to contact";
                        }

                        // Clear out the stored details
                        organizationDetail = QContactOrganization();
                    } else if (field.fieldName == QLatin1String("n")) {
                        if (!detailContext.isNull()) {
                            nameDetail.setContexts(detailContext);
                        }
                        nameDetail.setLastName(asString(field, 0));
                        nameDetail.setFirstName(asString(field, 1));
                        nameDetail.setMiddleName(asString(field, 2));
                        nameDetail.setPrefix(asString(field, 3));
                        nameDetail.setSuffix(asString(field, 4));
                    } else if (field.fieldName == QLatin1String("fn")) {
                        if (!detailContext.isNull()) {
                            nameDetail.setContexts(detailContext);
                        }
                        nameDetail.setCustomLabel(asString(field, 0));
                    } else if (field.fieldName == QLatin1String("nickname")) {
                        QContactNickname nicknameDetail;
                        nicknameDetail.setNickname(asString(field, 0));
                        if (!detailContext.isNull()) {
                            nicknameDetail.setContexts(detailContext);
                        }

                        if (!storeContactDetail(existingContact, nicknameDetail, SRC_LOC)) {
                            warning() << SRC_LOC << "Unable to save nickname to contact";
                        }
                    } else if (field.fieldName == QLatin1String("note") ||
                             field.fieldName == QLatin1String("desc")) {
                        QContactNote noteDetail;
                        if (!detailContext.isNull()) {
                            noteDetail.setContexts(detailContext);
                        }
                        noteDetail.setNote(asString(field, 0));

                        if (!storeContactDetail(existingContact, noteDetail, SRC_LOC)) {
                            warning() << SRC_LOC << "Unable to save note to contact";
                        }
                    } else if (field.fieldName == QLatin1String("bday")) {
                        /* FIXME: support more date format for compatibility */
                        const QString dateText(asString(field, 0));

                        QDate date = QDate::fromString(dateText, QLatin1String("yyyy-MM-dd"));
                        if (!date.isValid()) {
                            date = QDate::fromString(dateText, QLatin1String("yyyyMMdd"));
                        }
                        if (!date.isValid()) {
                            date = QDate::fromString(dateText, Qt::ISODate);
                        }

                        if (date.isValid()) {
                            QContactBirthday birthdayDetail;
                            birthdayDetail.setDate(date);

                            if (!storeContactDetail(existingContact, birthdayDetail, SRC_LOC)) {
                                warning() << SRC_LOC << "Unable to save birthday to contact";
                            }
                        } else {
                            debug() << "Unsupported bday format:" << field.fieldValue[0];
                        }
                    } else if (field.fieldName == QLatin1String("x-gender")) {
                        const QString type(field.fieldValue.at(0));

                        Dictionary::const_iterator it = genderTypes().find(type.toLower());
                        if (it != addressTypes().constEnd()) {
                            QContactGender genderDetail;
                            genderDetail.setGender(*it);

                            if (!storeContactDetail(existingContact, genderDetail, SRC_LOC)) {
                                warning() << SRC_LOC << "Unable to save gender to contact";
                            }
                        } else {
                            debug() << "Unsupported gender type:" << type;
                        }
                    } else {
                        debug() << "Unsupported contact info field" << field.fieldName;
                    }
                }

                if (!nameDetail.isEmpty()) {
                    if (!storeContactDetail(existingContact, nameDetail, SRC_LOC)) {
                        warning() << SRC_LOC << "Unable to save name details to contact";
                    }
                }
            }
        }
    }
    if (changes & CDTpContact::Avatar) {
        QString defaultAvatarPath = contact->avatarData().fileName;
        if (defaultAvatarPath.isEmpty()) {
            defaultAvatarPath = contactWrapper->squareAvatarPath();
        }

        QContactOnlineAccount qcoa = existingContact.detail<QContactOnlineAccount>();
        updateContactAvatars(existingContact, defaultAvatarPath, contactWrapper->largeAvatarPath(), qcoa);
    }
    if (changes & CDTpContact::DefaultAvatar) {
        updateSocialAvatars(network, contactWrapper);
    }
    /* What is this about?
    if (changes & CDTpContact::Authorization) {
        debug() << "  authorization changed";
        g.addPattern(imAddress, nco::imAddressAuthStatusFrom::resource(),
                presenceState(contact->subscriptionState()));
        g.addPattern(imAddress, nco::imAddressAuthStatusTo::resource(),
                presenceState(contact->publishState()));
    }
    */
}

template<typename T, typename R>
QList<R> forEachItem(const QList<T> &list, R (*f)(const T&))
{
    QList<R> rv;
    rv.reserve(list.count());

    foreach (const T &item, list) {
        const R& r = f(item);
        rv.append(r);
    }

    return rv;
}

QString extractAccountPath(const CDTpAccountPtr &accountWrapper)
{
    return imAccount(accountWrapper);
}

void addIconPath(QContactOnlineAccount &qcoa, Tp::AccountPtr account)
{
    QString iconName = account->iconName().trimmed();

    // Ignore any default value returned by telepathy
    if (!iconName.startsWith(QLatin1String("im-"))) {
        qcoa.setValue(QContactOnlineAccount__FieldAccountIconPath, iconName);
    }
}

} // namespace


CDTpStorage::CDTpStorage(QObject *parent) : QObject(parent),
    mUpdateRunning(false)
{
    mUpdateTimer.setInterval(UPDATE_TIMEOUT);
    mUpdateTimer.setSingleShot(true);
    connect(&mUpdateTimer, SIGNAL(timeout()), SLOT(onUpdateQueueTimeout()));
}

CDTpStorage::~CDTpStorage()
{
}

void CDTpStorage::addNewAccount(QContact &self, CDTpAccountPtr accountWrapper)
{
    Tp::AccountPtr account = accountWrapper->account();

    const QString accountPath(imAccount(account));
    const QString accountAddress(imAddress(account));
    const QString accountPresence(imPresence(account));

    debug() << "Creating new self account - account:" << accountPath << "address:" << accountAddress;

    // Create a new QCOA for this account
    QContactOnlineAccount newAccount;

    newAccount.setDetailUri(accountAddress);
    newAccount.setLinkedDetailUris(accountPresence);

    newAccount.setValue(QContactOnlineAccount__FieldAccountPath, accountPath);
    newAccount.setValue(QContactOnlineAccount__FieldEnabled, asString(account->isEnabled()));
    newAccount.setAccountUri(account->normalizedName());
    newAccount.setProtocol(account->protocolName());
    newAccount.setServiceProvider(account->serviceName());

    addIconPath(newAccount, account);

    // Add the new account to the self contact
    if (!storeContactDetail(self, newAccount, SRC_LOC)) {
        warning() << SRC_LOC << "Unable to add account to self contact for:" << accountPath;
        return;
    }

    // Create a presence detail for this account
    QContactPresence presence;

    presence.setDetailUri(accountPresence);
    presence.setLinkedDetailUris(accountAddress);
    presence.setPresenceState(qContactPresenceState(Tp::ConnectionPresenceTypeUnknown));

    if (!storeContactDetail(self, presence, SRC_LOC)) {
        warning() << SRC_LOC << "Unable to add presence to self contact for:" << accountPath;
        return;
    }

    // Store any information from the account
    CDTpContact::Changes selfChanges = updateAccountDetails(self, newAccount, presence, accountWrapper, CDTpAccount::All);

    if (!storeContact(self, SRC_LOC, selfChanges)) {
        warning() << SRC_LOC << "Unable to save self contact - error:" << manager()->error();
    }
}

void CDTpStorage::removeExistingAccount(QContact &self, QContactOnlineAccount &existing)
{
    const QString accountPath(existing.value(QContactOnlineAccount__FieldAccountPath));

    // Remove any contacts derived from this account
    if (!manager()->removeContacts(findContactIdsForAccount(accountPath))) {
        warning() << SRC_LOC << "Unable to remove linked contacts for account:" << accountPath << "error:" << manager()->error();
    }

    // Remove any details linked from the account
    QStringList linkedUris(existing.linkedDetailUris());

    foreach (QContactDetail detail, self.details()) {
        const QString &uri(detail.detailUri());
        if (!uri.isEmpty()) {
            if (linkedUris.contains(uri)) {
                if (!self.removeDetail(&detail)) {
                    warning() << SRC_LOC << "Unable to remove linked detail with URI:" << uri;
                }
            }
        }
    }

    if (!self.removeDetail(&existing)) {
        warning() << SRC_LOC << "Unable to remove obsolete account:" << accountPath;
    }
}

bool CDTpStorage::addNewContact(QContact &newContact, CDTpAccountPtr accountWrapper, const QString &contactId)
{
    Tp::AccountPtr account = accountWrapper->account();

    const QString accountPath(imAccount(account));
    const QString contactAddress(imAddress(account, contactId));
    const QString contactPresence(imPresence(account, contactId));

    debug() << "Creating new contact - address:" << contactAddress;

    // This contact is synchronized with telepathy
    QContactSyncTarget syncTarget;
    syncTarget.setSyncTarget(QLatin1String("telepathy"));
    if (!storeContactDetail(newContact, syncTarget, SRC_LOC)) {
        warning() << SRC_LOC << "Unable to add sync target to contact:" << contactAddress;
        return false;
    }

    // Create a metadata field to link the contact with the telepathy data
    QContactTpMetadata metadata;
    metadata.setContactId(contactAddress);
    metadata.setAccountId(imAccount(account));
    metadata.setAccountEnabled(true);
    if (!storeContactDetail(newContact, metadata, SRC_LOC)) {
        warning() << SRC_LOC << "Unable to add metadata to contact:" << contactAddress;
        return false;
    }

    // Create a new QCOA for this contact
    QContactOnlineAccount newAccount;

    newAccount.setDetailUri(contactAddress);
    newAccount.setLinkedDetailUris(contactPresence);

    newAccount.setValue(QContactOnlineAccount__FieldAccountPath, accountPath);
    newAccount.setValue(QContactOnlineAccount__FieldEnabled, asString(true));
    newAccount.setAccountUri(contactId);
    newAccount.setProtocol(account->protocolName());
    newAccount.setServiceProvider(account->serviceName());

    addIconPath(newAccount, account);

    // Add the new account to the contact
    if (!storeContactDetail(newContact, newAccount, SRC_LOC)) {
        warning() << SRC_LOC << "Unable to save account to contact for:" << contactAddress;
        return false;
    }

    // Create a presence detail for this contact
    QContactPresence presence;

    presence.setDetailUri(contactPresence);
    presence.setLinkedDetailUris(contactAddress);
    presence.setPresenceState(qContactPresenceState(Tp::ConnectionPresenceTypeUnknown));

    if (!storeContactDetail(newContact, presence, SRC_LOC)) {
        warning() << SRC_LOC << "Unable to save presence to contact for:" << contactAddress;
        return false;
    }

    if (!storeContact(newContact, SRC_LOC)) {
        warning() << SRC_LOC << "Unable to save contact:" << contactAddress << "- error:" << manager()->error();
        return false;
    }

    return true;
}

void CDTpStorage::updateContactChanges(CDTpContactPtr contactWrapper, CDTpContact::Changes changes)
{
    const QString accountPath(imAccount(contactWrapper));
    const QString contactAddress(imAddress(contactWrapper));
    const QString contactPresence(imPresence(contactWrapper));

    QContact existingContact = findExistingContact(contactAddress);

    if (changes & CDTpContact::Deleted) {
        // This contact has been deleted
        if (!existingContact.isEmpty()) {
            if (!manager()->removeContact(existingContact.localId())) {
                warning() << SRC_LOC << "Unable to remove deleted contact for account:" << accountPath << "- error:" << manager()->error();
            }
        }
    } else {
        if (existingContact.isEmpty()) {
            if (!addNewContact(existingContact, contactWrapper->accountWrapper(), contactWrapper->contact()->id())) {
                warning() << SRC_LOC << "Unable to create contact for account:" << accountPath << contactAddress;
                return;
            }
        }

        updateContactDetails(mNetwork, existingContact, contactWrapper, changes);

        if (!storeContact(existingContact, SRC_LOC, changes)) {
            warning() << SRC_LOC << "Unable to save new contact for:" << contactAddress << "- error:" << manager()->error();
        }
    }
}

void CDTpStorage::updateAccountChanges(QContactOnlineAccount &qcoa, CDTpAccountPtr accountWrapper, CDTpAccount::Changes changes)
{
    QContact self(selfContact());
    if (self.isEmpty()) {
        warning() << SRC_LOC << "Unable to retrieve self contact - error:" << manager()->error();
        return;
    }

    Tp::AccountPtr account = accountWrapper->account();

    const QString accountPath(imAccount(account));
    const QString accountAddress(imAddress(account));

    debug() << "Synchronizing self account - account:" << accountPath << "address:" << accountAddress;

    QContactPresence presence(findPresenceForAccount(self, qcoa));
    if (presence.isEmpty()) {
        warning() << SRC_LOC << "Unable to find presence to match account:" << accountPath;
    }
    CDTpContact::Changes selfChanges = updateAccountDetails(self, qcoa, presence, accountWrapper, changes);

    if (!storeContact(self, SRC_LOC, selfChanges)) {
        warning() << SRC_LOC << "Unable to save self contact - error:" << manager()->error();
    }

    if (account->isEnabled() && accountWrapper->hasRoster()) {
        QHash<QString, CDTpContact::Changes> allChanges;

        // Update all contacts reported in the roster changes of this account
        const QHash<QString, CDTpContact::Changes> changes = accountWrapper->rosterChanges();
        QHash<QString, CDTpContact::Changes>::ConstIterator it = changes.constBegin(), end = changes.constEnd();
        for ( ; it != end; ++it) {
            const QString address = imAddress(accountPath, it.key());

            // We always update contact presence since this method is called after a presence change
            allChanges.insert(address, it.value() | CDTpContact::Presence);
        }

        foreach (CDTpContactPtr contactWrapper, accountWrapper->contacts()) {
            const QString address = imAddress(accountPath, contactWrapper->contact()->id());
            QHash<QString, CDTpContact::Changes>::Iterator changes = allChanges.find(address);

            // Should never happen
            if (changes == allChanges.end()) {
                warning() << SRC_LOC << "No changes found for contact:" << address;
                continue;
            }

            // If we got a contact without avatar in the roster, and the original
            // had an avatar, then ignore the avatar update (some contact managers
            // send the initial roster with the avatar missing)
            // Contact updates that have a null avatar will clear the avatar though
            if (*changes & CDTpContact::DefaultAvatar) {
                if (*changes != CDTpContact::Added
                  && contactWrapper->contact()->avatarData().fileName.isEmpty()) {
                    *changes ^= CDTpContact::DefaultAvatar;
                }
            }

            updateContactChanges(contactWrapper, *changes);
        }
    } else {
        // Set presence to unknown for all contacts of this account
        foreach (const QContactLocalId &contactId, findContactIdsForAccount(accountPath)) {
            QContact existingContact = manager()->contact(contactId);

            QContactPresence presence = existingContact.detail<QContactPresence>();
            presence.setPresenceState(qContactPresenceState(Tp::ConnectionPresenceTypeUnknown));
            presence.setTimestamp(QDateTime::currentDateTime());

            if (!storeContactDetail(existingContact, presence, SRC_LOC)) {
                warning() << SRC_LOC << "Unable to save unknown presence to contact for:" << contactId;
            }

            // Also reset the capabilities
            QContactOnlineAccount qcoa = existingContact.detail<QContactOnlineAccount>();
            qcoa.setCapabilities(currentCapabilites(account->capabilities(), Tp::ConnectionPresenceTypeUnknown, account));

            if (!storeContactDetail(existingContact, qcoa, SRC_LOC)) {
                warning() << SRC_LOC << "Unable to save capabilities to contact for:" << contactId;
            }

            if (!account->isEnabled()) {
                // Mark the contact as un-enabled also
                QContactTpMetadata metadata = existingContact.detail<QContactTpMetadata>();
                metadata.setAccountEnabled(false);

                if (!storeContactDetail(existingContact, metadata, SRC_LOC)) {
                    warning() << SRC_LOC << "Unable to un-enable contact for:" << contactId;
                }
            }

            if (!storeContact(existingContact, SRC_LOC, CDTpContact::Presence | CDTpContact::Capabilities)) {
                warning() << SRC_LOC << "Unable to save account contact - error:" << manager()->error();
            }
        }
    }
}

void CDTpStorage::syncAccounts(const QList<CDTpAccountPtr> &accounts)
{
    QContact self(selfContact());
    if (self.isEmpty()) {
        warning() << SRC_LOC << "Unable to retrieve self contact - error:" << manager()->error();
        return;
    }

    // Find the list of paths for the accounts we now have
    QStringList accountPaths = forEachItem(accounts, extractAccountPath);
    
    QSet<int> existingIndices;

    foreach (QContactOnlineAccount existingAccount, self.details<QContactOnlineAccount>()) {
        const QString existingPath(existingAccount.value(QContactOnlineAccount__FieldAccountPath));
        if (existingPath.isEmpty()) {
            warning() << SRC_LOC << "No path for existing account:" << existingPath;
            continue;
        }

        int index = accountPaths.indexOf(existingPath);
        if (index != -1) {
            existingIndices.insert(index);
            updateAccountChanges(existingAccount, accounts.at(index), CDTpAccount::All);
        } else {
            debug() << SRC_LOC << "Remove obsolete account:" << existingPath;

            // This account is no longer valid
            removeExistingAccount(self, existingAccount);
        }
    }

    // Add any previously unknown accounts
    for (int i = 0; i < accounts.length(); ++i) {
        if (!existingIndices.contains(i)) {
            addNewAccount(self, accounts.at(i));
        }
    }

    if (!storeContact(self, SRC_LOC)) {
        warning() << SRC_LOC << "Unable to save self contact";
    }
}

void CDTpStorage::createAccount(CDTpAccountPtr accountWrapper)
{
    QContact self(selfContact());
    if (self.isEmpty()) {
        warning() << SRC_LOC << "Unable to retrieve self contact:" << manager()->error();
        return;
    }

    const QString accountPath(imAccount(accountWrapper));

    debug() << SRC_LOC << "Create account:" << accountPath;

    // Ensure this account does not already exist
    foreach (const QContactOnlineAccount &existingAccount, self.details<QContactOnlineAccount>()) {
        const QString existingPath(existingAccount.value(QContactOnlineAccount__FieldAccountPath));
        if (existingPath == accountPath) {
            warning() << SRC_LOC << "Path already exists for create account:" << existingPath;
            return;
        }
    }

    // Add any previously unknown accounts
    addNewAccount(self, accountWrapper);

    // Add any contacts already present for this account
    foreach (CDTpContactPtr contactWrapper, accountWrapper->contacts()) {
        updateContactChanges(contactWrapper, CDTpContact::All);
    }

    if (!storeContact(self, SRC_LOC)) {
        warning() << SRC_LOC << "Unable to save self contact";
    }
}

void CDTpStorage::updateAccount(CDTpAccountPtr accountWrapper, CDTpAccount::Changes changes)
{
    QContact self(selfContact());
    if (self.isEmpty()) {
        warning() << SRC_LOC << "Unable to retrieve self contact:" << manager()->error();
        return;
    }

    const QString accountPath(imAccount(accountWrapper));

    debug() << SRC_LOC << "Update account:" << accountPath;

    foreach (QContactOnlineAccount existingAccount, self.details<QContactOnlineAccount>()) {
        const QString existingPath(existingAccount.value(QContactOnlineAccount__FieldAccountPath));
        if (existingPath == accountPath) {
            updateAccountChanges(existingAccount, accountWrapper, changes);

            if (!storeContact(self, SRC_LOC)) {
                warning() << SRC_LOC << "Unable to save self contact";
            }
            return;
        }
    }

    warning() << SRC_LOC << "Account not found for update account:" << accountPath;
}

void CDTpStorage::removeAccount(CDTpAccountPtr accountWrapper)
{
    cancelQueuedUpdates(accountWrapper->contacts());

    QContact self(selfContact());
    if (self.isEmpty()) {
        warning() << SRC_LOC << "Unable to retrieve self contact:" << manager()->error();
        return;
    }

    const QString accountPath(imAccount(accountWrapper));

    debug() << SRC_LOC << "Remove account:" << accountPath;

    foreach (QContactOnlineAccount existingAccount, self.details<QContactOnlineAccount>()) {
        const QString existingPath(existingAccount.value(QContactOnlineAccount__FieldAccountPath));
        if (existingPath == accountPath) {
            removeExistingAccount(self, existingAccount);

            if (!storeContact(self, SRC_LOC)) {
                warning() << SRC_LOC << "Unable to save self contact";
            }
            return;
        }
    }

    warning() << SRC_LOC << "Account not found for remove account:" << accountPath;
}

// This is called when account goes online/offline
void CDTpStorage::syncAccountContacts(CDTpAccountPtr accountWrapper)
{
    QContact self(selfContact());
    if (self.isEmpty()) {
        warning() << SRC_LOC << "Unable to retrieve self contact:" << manager()->error();
        return;
    }

    const QString accountPath(imAccount(accountWrapper));

    debug() << SRC_LOC << "Sync contacts account:" << accountPath;

    foreach (QContactOnlineAccount existingAccount, self.details<QContactOnlineAccount>()) {
        const QString existingPath(existingAccount.value(QContactOnlineAccount__FieldAccountPath));
        if (existingPath == accountPath) {
            updateAccountChanges(existingAccount, accountWrapper, CDTpAccount::Enabled);

            if (!storeContact(self, SRC_LOC)) {
                warning() << SRC_LOC << "Unable to save self contact";
            }
            return;
        }
    }

    warning() << SRC_LOC << "Account not found for sync account:" << accountPath;
}

void CDTpStorage::syncAccountContacts(CDTpAccountPtr accountWrapper, const QList<CDTpContactPtr> &contactsAdded, const QList<CDTpContactPtr> &contactsRemoved)
{
    const QString accountPath(imAccount(accountWrapper));

    foreach (const CDTpContactPtr &contactWrapper, contactsAdded) {
        // This contact should be for the specified account
        if (imAccount(contactWrapper) != accountPath) {
            warning() << SRC_LOC << "Unable to add contact from wrong account:" << imAccount(contactWrapper) << accountPath;
            continue;
        }

        updateContactChanges(contactWrapper, CDTpContact::Added | CDTpContact::Information);
    }

    foreach (const CDTpContactPtr &contactWrapper, contactsRemoved) {
        // This contact should be for the specified account
        if (imAccount(contactWrapper) != accountPath) {
            warning() << SRC_LOC << "Unable to remove contact from wrong account:" << imAccount(contactWrapper) << accountPath;
            continue;
        }

        updateContactChanges(contactWrapper, CDTpContact::Deleted);
    }
}

void CDTpStorage::createAccountContacts(CDTpAccountPtr accountWrapper, const QStringList &imIds, uint localId)
{
    Q_UNUSED(localId) // ???

    const QString accountPath(imAccount(accountWrapper));

    debug() << SRC_LOC << "Create contacts account:" << accountPath;

    QStringList imAddressList;
    foreach (const QString &id, imIds) {
        QContact newContact;
        if (!addNewContact(newContact, accountWrapper, id)) {
            warning() << SRC_LOC << "Unable to create contact for account:" << accountPath << id;
        }
    }
}

/* Use this only in offline mode - use syncAccountContacts in online mode */
void CDTpStorage::removeAccountContacts(CDTpAccountPtr accountWrapper, const QStringList &contactIds)
{
    const QString accountPath(imAccount(accountWrapper));

    debug() << SRC_LOC << "Remove contacts account:" << accountPath;

    QStringList imAddressList;
    foreach (const QString &id, contactIds) {
        imAddressList.append(imAddress(accountPath, id));
    }

    QList<QContactLocalId> removeIds;

    // Find any contacts matching the supplied ID list
    foreach (const QContact &existingContact, manager()->contacts(findContactIdsForAccount(accountPath))) {
        QContactTpMetadata metadata = existingContact.detail<QContactTpMetadata>();
        if (imAddressList.contains(metadata.contactId())) {
            removeIds.append(existingContact.localId());
        }
    }

    if (!manager()->removeContacts(removeIds)) {
        warning() << SRC_LOC << "Unable to remove contacts for account:" << accountPath << "error:" << manager()->error();
    }
}

void CDTpStorage::updateContact(CDTpContactPtr contactWrapper, CDTpContact::Changes changes)
{
    mUpdateQueue[contactWrapper] |= changes;

    if (!mUpdateRunning) {
        // Only update IM contacts in tracker after queuing 50 contacts or after
        // not receiving an update notifiction for 150 ms. This dramatically reduces
        // system but also keeps update latency within acceptable bounds.
        if (!mUpdateTimer.isActive() || mUpdateQueue.count() < UPDATE_THRESHOLD) {
            mUpdateTimer.start();
        }
    }
}

void CDTpStorage::onUpdateQueueTimeout()
{
    debug() << "Update" << mUpdateQueue.count() << "contacts";

    QHash<CDTpContactPtr, CDTpContact::Changes>::const_iterator it = mUpdateQueue.constBegin(), end = mUpdateQueue.constEnd();
    for ( ; it != end; ++it) {
        CDTpContactPtr contactWrapper = it.key();

        // Skip the contact in case its account was deleted before this function
        // was invoked
        if (contactWrapper->accountWrapper().isNull()) {
            continue;
        }
        if (!contactWrapper->isVisible()) {
            continue;
        }

        updateContactChanges(contactWrapper, it.value());
    }

    mUpdateQueue.clear();
}

void CDTpStorage::cancelQueuedUpdates(const QList<CDTpContactPtr> &contacts)
{
    foreach (const CDTpContactPtr &contactWrapper, contacts) {
        mUpdateQueue.remove(contactWrapper);
    }
}

