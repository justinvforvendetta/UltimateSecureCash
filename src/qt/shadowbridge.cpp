#include "shadowbridge.h"

#include "shadowgui.h"
#include "guiutil.h"

#include "editaddressdialog.h"

#include "transactiontablemodel.h"
#include "transactionrecord.h"
#include "transactiondesc.h"

#include "addressbookpage.h"
#include "addresstablemodel.h"


#include "messagemodel.h"

#include "clientmodel.h"
#include "walletmodel.h"
#include "messagemodel.h"
#include "optionsmodel.h"

#include "bitcoinunits.h"
#include "coincontrol.h"
#include "coincontroldialog.h"
#include "ringsig.h"

#include "askpassphrasedialog.h"

#include <QApplication>
#include <QThread>
#include <QWebFrame>
#include <QClipboard>
#include <QMessageBox>
#include <QSortFilterProxyModel>

#include <QVariantList>
#include <QVariantMap>

#include <QDir>

#define ROWS_TO_REFRESH 200

extern CWallet* pwalletMain;

class TransactionModel : public QObject
{
    Q_OBJECT

public:
    TransactionModel(QObject *parent = 0) :
        ttm(new QSortFilterProxyModel()),
        running(false),
        QObject(parent)
    { }

    ~TransactionModel()
    {
        delete ttm;
    }

    void init(ClientModel * clientModel, TransactionTableModel * transactionTableModel)
    {
        this->clientModel = clientModel;

        ttm->setSourceModel(transactionTableModel);
        ttm->setSortRole(Qt::EditRole);
        ttm->sort(TransactionTableModel::Status, Qt::DescendingOrder);
    }

    QVariantMap addTransaction(int row)
    {
        QModelIndex status   = ttm->index    (row, TransactionTableModel::Status);
        QModelIndex date     = status.sibling(row, TransactionTableModel::Date);
        QModelIndex address  = status.sibling(row, TransactionTableModel::ToAddress);
        QModelIndex amount   = status.sibling(row, TransactionTableModel::Amount);
        QVariantMap transaction;

        transaction.insert("id",   status.data(TransactionTableModel::TxIDRole).toString());
        transaction.insert("tt",   status.data(Qt::ToolTipRole).toString());
        transaction.insert("c",    status.data(TransactionTableModel::ConfirmationsRole).toLongLong());
        transaction.insert("s",    status.data(Qt::DecorationRole).toString());
        transaction.insert("d",    date.data(Qt::EditRole).toInt());
        transaction.insert("d_s",  date.data().toString());
        transaction.insert("t",    TransactionRecord::getTypeShort(status.data(TransactionTableModel::TypeRole).toInt()));
        transaction.insert("t_l",  status.sibling(row, TransactionTableModel::Type).data().toString());
        transaction.insert("ad_c", address.data(Qt::ForegroundRole).value<QColor>().name());
        transaction.insert("ad",   address.data(TransactionTableModel::AddressRole).toString());
        transaction.insert("ad_l", address.data(TransactionTableModel::LabelRole).toString());
        transaction.insert("ad_d", address.data().toString());
        transaction.insert("n",    status.sibling(row, TransactionTableModel::Narration).data().toString());
        transaction.insert("am_c", amount.data(Qt::ForegroundRole).value<QColor>().name());
        transaction.insert("am",   amount.data(TransactionTableModel::AmountRole).toString());
        transaction.insert("am_d", amount.data().toString());

        return transaction;
    }

    void populateRows(int start, int end)
    {

        if(start > ROWS_TO_REFRESH)
            return;

        if(!prepare())
            return;

        if (end > ROWS_TO_REFRESH)
            end = ROWS_TO_REFRESH;

        QVariantList transactions;

        while(start <= end)
        {
            if(visibleTransactions.first() == "*"||visibleTransactions.contains(ttm->index(start, TransactionTableModel::Type).data().toString()))
                transactions.append(addTransaction(start));

            start++;
        }

        if(!transactions.isEmpty())
            emitTransactions(transactions);

        running = false;
    }

    void populatePage()
    {
        if(!prepare(false))
            return;

        QVariantList transactions;

        int row = -1;

        running = true;

        while(++row < numRows && ttm->index(row, 0).isValid())
            if(visibleTransactions.first() == "*"||visibleTransactions.contains(ttm->index(row, TransactionTableModel::Type).data().toString()))
                transactions.append(addTransaction(row));

        if(!transactions.isEmpty())
            emitTransactions(transactions);

        running = false;
    }

    QSortFilterProxyModel * getModel()
    {
        return ttm;
    }

    bool isRunning() {
        return running;
    }

signals:
    void emitTransactions(const QVariantList & transactions, bool reset = false);

private:
    ClientModel *clientModel;
    QSortFilterProxyModel *ttm;
    QStringList visibleTransactions;
    int numRows;
    int rowsPerPage;
    bool running;

    bool prepare(bool running=true)
    {
        if(this->running || (running && clientModel->inInitialBlockDownload()))
            return false;

        numRows = ttm->rowCount();
        ttm->sort(TransactionTableModel::Status, Qt::DescendingOrder);
        rowsPerPage = clientModel->getOptionsModel()->getRowsPerPage();
        visibleTransactions = clientModel->getOptionsModel()->getVisibleTransactions();

        this->running = running;

        return true;
    }
};


class AddressModel : public QObject
{
    Q_OBJECT

public:
    AddressTableModel *atm;

    QVariantMap addAddress(int row)
    {
        QVariantMap address;
        QModelIndex label = atm->index(row, AddressTableModel::Label);

        address.insert("type",        label.data(AddressTableModel::TypeRole).toString());
        address.insert("label_value", label.data(Qt::EditRole).toString());
        address.insert("label",       label.data().toString());
        address.insert("address",     label.sibling(row, AddressTableModel::Address).data().toString());
        address.insert("pubkey",      label.sibling(row, AddressTableModel::Pubkey).data().toString());

        return address;
    }

    void poplateRows(int start, int end)
    {
        QVariantList addresses;

        while(start <= end)
        {
            if(!atm->index(start, 0).isValid())
                continue;

            addresses.append(addAddress(start++));
        }

        emitAddresses(addresses);
    }

    void populateAddressTable()
    {
        running = true;

        int row = -1;
        int end = atm->rowCount();
        QVariantList addresses;

        while(++row < end)
        {
            if(!atm->index(row, 0).isValid())
                continue;

            addresses.append(addAddress(row));
        }

        emitAddresses(addresses, true);

        running = false;
    }

    bool isRunning() {
        return running;
    }

signals:
    void emitAddresses(const QVariantList & addresses, bool reset = false);

private:
    bool running;
};


class MessageThread : public QThread
{
    Q_OBJECT

signals:
    void emitMessages(const QString & messages, bool reset);

public:
    MessageModel *mtm;

    QString addMessage(int row)
    {
        return QString("{\"id\":\"%10\",\"type\":\"%1\",\"sent_date\":\"%2\",\"received_date\":\"%3\", \"label_value\":\"%4\",\"label\":\"%5\",\"to_address\":\"%6\",\"from_address\":\"%7\",\"message\":\"%8\",\"read\":%9},")
                .arg(mtm->index(row, MessageModel::Type)            .data().toString())
                .arg(mtm->index(row, MessageModel::SentDateTime)    .data().toDateTime().toTime_t())
                .arg(mtm->index(row, MessageModel::ReceivedDateTime).data().toDateTime().toTime_t())
                .arg(mtm->index(row, MessageModel::Label)           .data(MessageModel::LabelRole).toString())
                .arg(mtm->index(row, MessageModel::Label)           .data().toString().replace("\\", "\\\\").replace("/", "\\/").replace("\"","\\\""))
                .arg(mtm->index(row, MessageModel::ToAddress)       .data().toString())
                .arg(mtm->index(row, MessageModel::FromAddress)     .data().toString())
                .arg(mtm->index(row, MessageModel::Message)         .data().toString().toHtmlEscaped().replace("\\", "\\\\").replace("\"","\\\"").replace("\n", "\\n"))
                .arg(mtm->index(row, MessageModel::Read)            .data().toBool())
                .arg(mtm->index(row, MessageModel::Key)             .data().toString());
    }

protected:
    void run()
    {
        int row = -1;
        QString messages;
        while (mtm->index(++row, 0, QModelIndex()).isValid())
            messages.append(addMessage(row));

        emitMessages(messages, true);
    }

};

#include "shadowbridge.moc"

ShadowBridge::ShadowBridge(ShadowGUI *window, QObject *parent) :
    window          (window),
    transactionModel(new TransactionModel()),
    addressModel    (new AddressModel()),
    thMessage       (new MessageThread()),
    info            (new QVariantMap()),
    async           (new QThread()),
    QObject         (parent)
{
    async->start();
}

ShadowBridge::~ShadowBridge()
{
    delete transactionModel;
    delete addressModel;
    delete thMessage;
    delete async;
    delete info;
}

// This is just a hook, we won't really be setting the model...
void ShadowBridge::setClientModel()
{

    info->insert("version", CLIENT_VERSION);
    info->insert("build",   window->clientModel->formatFullVersion());
    info->insert("date",    window->clientModel->formatBuildDate());
    info->insert("name",    window->clientModel->clientName());

    populateOptions();
}

// This is just a hook, we won't really be setting the model...
void ShadowBridge::setWalletModel()
{
    populateTransactionTable();
    populateAddressTable();

    connect(window->clientModel->getOptionsModel(), SIGNAL(visibleTransactionsChanged(QStringList)), SLOT(populateTransactionTable()));
}

// This is just a hook, we won't really be setting the model...
void ShadowBridge::setMessageModel()
{
    populateMessageTable();
    connectSignals();
}

void ShadowBridge::copy(QString text)
{
    QApplication::clipboard()->setText(text);
}

void ShadowBridge::paste() {
    emitPaste(QApplication::clipboard()->text());
}

// Options
void ShadowBridge::populateOptions()
{
    OptionsModel * optionsModel(window->clientModel->getOptionsModel());

    int option = 0;

    QVariantMap options;

    for(option=0;option < optionsModel->rowCount(); option++)
        options.insert(optionsModel->optionIDName(option), optionsModel->index(option).data(Qt::EditRole));

    option = 0;

    QVariantList visibleTransactions;
    QVariantMap notifications;

    while(true)
    {
        QString txType(TransactionRecord::getTypeLabel(option++));

        if(txType.isEmpty())
            break;

        if(visibleTransactions.contains(txType))
        {
            if(txType.isEmpty())
            {
                if(visibleTransactions.length() != 0)
                    break;
            }
            else
                continue;
        }

        visibleTransactions.append(txType);
    }

    QVariantList messageTypes;

    messageTypes.append(tr("Incoming Message"));
    notifications.insert("messages", messageTypes);
    notifications.insert("transactions", visibleTransactions);

    options.insert("optVisibleTransactions", visibleTransactions);
    options.insert("optNotifications",       notifications);

    /* Display elements init */
    QDir translations(":translations");

    QVariantMap languages;

    languages.insert("", "(" + tr("default") + ")");

    foreach(const QString &langStr, translations.entryList())
    {
        QLocale locale(langStr);

        /** display language strings as "native language [- native country] (locale name)", e.g. "Deutsch - Deutschland (de)" */
        languages.insert(langStr, locale.nativeLanguageName() + (langStr.contains("_") ? " - " + locale.nativeCountryName() : "") + " (" + langStr + ")");
    }

    options.insert("optLanguage", languages);

    info->insert("options", options);
}

// Transactions
bool ShadowBridge::addRecipient(QString address, QString label, QString narration, qint64 amount, int txnType, int nRingSize)
{
    SendCoinsRecipient rv;

    rv.address = address;
    rv.label = label;
    rv.narration = narration;
    rv.amount = amount;
    rv.typeInd = (address.length() > 75 ? AddressTableModel::AT_Stealth : AddressTableModel::AT_Normal);

    rv.txnTypeInd = txnType;
    rv.nRingSize = nRingSize;

    recipients.append(rv);

    return true;
}

void ShadowBridge::clearRecipients()
{
    recipients.clear();
}

bool ShadowBridge::sendCoins(bool fUseCoinControl, QString sChangeAddr)
{
    WalletModel::UnlockContext ctx(window->walletModel->requestUnlock());

    // Unlock wallet was cancelled
    if(!ctx.isValid())
        return false;

    int inputTypes = -1;
    int nAnonOutputs = 0;
    int ringSizes = -1;
    // Format confirmation message
    QStringList formatted;
    foreach(const SendCoinsRecipient &rcp, recipients)
    {
        int inputType; // 0 USC, 1 Shadow
        switch(rcp.txnTypeInd)
        {
            case TXT_USC_TO_USC:
                formatted.append(tr("<b>%1</b> to %2 (%3)").arg(BitcoinUnits::formatWithUnit(BitcoinUnits::USC, rcp.amount), Qt::escape(rcp.label), rcp.address));
                inputType = 0;
                break;
            case TXT_USC_TO_ANON:
                formatted.append(tr("<b>%1</b> to SHADOW %2 (%3)").arg(BitcoinUnits::formatWithUnit(BitcoinUnits::USC, rcp.amount), Qt::escape(rcp.label), rcp.address));
                inputType = 0;
                nAnonOutputs++;
                break;
            case TXT_ANON_TO_ANON:
                formatted.append(tr("<b>%1</b>, ring size %2 to SHADOW %3 (%4)").arg(BitcoinUnits::formatWithUnit(BitcoinUnits::USC, rcp.amount), QString::number(rcp.nRingSize), Qt::escape(rcp.label), rcp.address));
                inputType = 1;
                nAnonOutputs++;
                break;
            case TXT_ANON_TO_USC:
                formatted.append(tr("<b>%1</b>, ring size %2 to USC %3 (%4)").arg(BitcoinUnits::formatWithUnit(BitcoinUnits::USC, rcp.amount), QString::number(rcp.nRingSize), Qt::escape(rcp.label), rcp.address));
                inputType = 1;
                break;
            default:
                QMessageBox::critical(window, tr("Error:"), tr("Unknown txn type detected %1.").arg(rcp.txnTypeInd),
                              QMessageBox::Abort, QMessageBox::Abort);
                return false;
        }

        if (inputTypes == -1)
            inputTypes = inputType;
        else
        if (inputTypes != inputType)
        {
            QMessageBox::critical(window, tr("Error:"), tr("Input types must match for all recipients."),
                          QMessageBox::Abort, QMessageBox::Abort);
            return false;
        };

        if (inputTypes == 1)
        {
            if (ringSizes == -1)
                ringSizes = rcp.nRingSize;
            else
            if (ringSizes != rcp.nRingSize)
            {
                QMessageBox::critical(window, tr("Error:"), tr("Ring sizes must match for all recipients."),
                              QMessageBox::Abort, QMessageBox::Abort);
                return false;
            };

            if (ringSizes < (int)MIN_RING_SIZE
                || ringSizes > (int)MAX_RING_SIZE)
            {
                QMessageBox::critical(window, tr("Error:"), tr("Ring size outside range [%1, %2].").arg(MIN_RING_SIZE).arg(MAX_RING_SIZE),
                              QMessageBox::Abort, QMessageBox::Abort);
                return false;
            };

            if (ringSizes == 1)
            {
                QMessageBox::StandardButton retval = QMessageBox::question(window,
                    tr("Confirm send coins"), tr("Are you sure you want to send?\nRing size of one is not anonymous, and harms the network.").arg(formatted.join(tr(" and "))),
                    QMessageBox::Yes|QMessageBox::Cancel, QMessageBox::Cancel);
                if (retval != QMessageBox::Yes)
                    return false;
            };
        };
    };

    QMessageBox::StandardButton retval = QMessageBox::question(window,
        tr("Confirm send coins"), tr("Are you sure you want to send %1?").arg(formatted.join(tr(" and "))),
        QMessageBox::Yes|QMessageBox::Cancel, QMessageBox::Cancel);

    if(retval != QMessageBox::Yes)
        return false;

    WalletModel::SendCoinsReturn sendstatus;

    if (fUseCoinControl)
    {
        if (sChangeAddr.length() > 0)
        {
            CBitcoinAddress addrChange = CBitcoinAddress(sChangeAddr.toStdString());
            if (!addrChange.IsValid())
            {
                QMessageBox::warning(window, tr("Send Coins"),
                    tr("The change address is not valid, please recheck."),
                    QMessageBox::Ok, QMessageBox::Ok);
                return false;
            };

            CoinControlDialog::coinControl->destChange = addrChange.Get();
        } else
            CoinControlDialog::coinControl->destChange = CNoDestination();
    };

    if (inputTypes == 1 || nAnonOutputs > 0)
        sendstatus = window->walletModel->sendCoinsAnon(recipients, fUseCoinControl ? CoinControlDialog::coinControl : NULL);
    else
        sendstatus = window->walletModel->sendCoins    (recipients, fUseCoinControl ? CoinControlDialog::coinControl : NULL);

    switch(sendstatus.status)
    {
        case WalletModel::InvalidAddress:
            QMessageBox::warning(window, tr("Send Coins"),
                tr("The recipient address is not valid, please recheck."),
                QMessageBox::Ok, QMessageBox::Ok);
            return false;
        case WalletModel::InvalidAmount:
            QMessageBox::warning(window, tr("Send Coins"),
                tr("The amount to pay must be larger than 0."),
                QMessageBox::Ok, QMessageBox::Ok);
            return false;
        case WalletModel::AmountExceedsBalance:
            QMessageBox::warning(window, tr("Send Coins"),
                tr("The amount exceeds your balance."),
                QMessageBox::Ok, QMessageBox::Ok);
            return false;
        case WalletModel::AmountWithFeeExceedsBalance:
            QMessageBox::warning(window, tr("Send Coins"),
                tr("The total exceeds your balance when the %1 transaction fee is included.").
                arg(BitcoinUnits::formatWithUnit(BitcoinUnits::USC, sendstatus.fee)),
                QMessageBox::Ok, QMessageBox::Ok);
            return false;
        case WalletModel::DuplicateAddress:
            QMessageBox::warning(window, tr("Send Coins"),
                tr("Duplicate address found, can only send to each address once per send operation."),
                QMessageBox::Ok, QMessageBox::Ok);
            return false;
        case WalletModel::TransactionCreationFailed:
            QMessageBox::warning(window, tr("Send Coins"),
                tr("Error: Transaction creation failed."),
                QMessageBox::Ok, QMessageBox::Ok);
            return false;
        case WalletModel::TransactionCommitFailed:
            QMessageBox::warning(window, tr("Send Coins"),
                tr("Error: The transaction was rejected. This might happen if some of the coins in your wallet were already spent, such as if you used a copy of wallet.dat and coins were spent in the copy but not marked as spent here."),
                QMessageBox::Ok, QMessageBox::Ok);
            return false;
        case WalletModel::NarrationTooLong:
            QMessageBox::warning(window, tr("Send Coins"),
                tr("Error: Narration is too long."),
                QMessageBox::Ok, QMessageBox::Ok);
            return false;
        case WalletModel::RingSizeError:
            QMessageBox::warning(window, tr("Send Coins"),
                tr("Error: Ring Size Error."),
                QMessageBox::Ok, QMessageBox::Ok);
            return false;
        case WalletModel::InputTypeError:
            QMessageBox::warning(window, tr("Send Coins"),
                tr("Error: Input Type Error."),
                QMessageBox::Ok, QMessageBox::Ok);
            return false;
        case WalletModel::SCR_NeedFullMode:
            QMessageBox::warning(window, tr("Send Coins"),
                tr("Error: Must be in full mode to send anon."),
                QMessageBox::Ok, QMessageBox::Ok);
            return false;
        case WalletModel::SCR_StealthAddressFail:
            QMessageBox::warning(window, tr("Send Coins"),
                tr("Error: Invalid Stealth Address."),
                QMessageBox::Ok, QMessageBox::Ok);
            return false;
        case WalletModel::SCR_AmountWithFeeExceedsShadowBalance:
            QMessageBox::warning(window, tr("Send Coins"),
                tr("The total exceeds your shadow balance when the %1 transaction fee is included.").arg(BitcoinUnits::formatWithUnit(BitcoinUnits::USC, sendstatus.fee)),
                QMessageBox::Ok, QMessageBox::Ok);
            return false;
        case WalletModel::SCR_Error:
            QMessageBox::warning(window, tr("Send Coins"),
                tr("Error generating transaction."),
                QMessageBox::Ok, QMessageBox::Ok);
            return false;
        case WalletModel::SCR_ErrorWithMsg:
            QMessageBox::warning(window, tr("Send Coins"),
                tr("Error generating transaction: %1").arg(sendstatus.hex),
                QMessageBox::Ok, QMessageBox::Ok);
            return false;

        case WalletModel::Aborted: // User aborted, nothing to do
            return false;
        case WalletModel::OK:
            //accept();
            CoinControlDialog::coinControl->UnSelectAll();
            CoinControlDialog::payAmounts.clear();
            CoinControlDialog::updateLabels(window->walletModel, 0, this);
            recipients.clear();
            break;
    }

    return true;
}

void ShadowBridge::openAddressBook(bool sending)
{
    if (!window || !addressModel->atm)
        return;

    AddressBookPage dlg(AddressBookPage::ForSending, sending ? AddressBookPage::SendingTab : AddressBookPage::ReceivingTab, window);

    dlg.setModel(addressModel->atm);

    if (dlg.exec())
    {
        QString address = dlg.getReturnValue();
        QString label = addressModel->atm->labelForAddress(address);

        emitAddressBookReturn(address, label);
    }

}

void ShadowBridge::openCoinControl()
{
    if (!window || !window->walletModel)
        return;

    CoinControlDialog dlg;
    dlg.setModel(window->walletModel);
    dlg.exec();

    CoinControlDialog::updateLabels(window->walletModel, 0, this);
}

void ShadowBridge::updateCoinControlAmount(qint64 amount)
{
    CoinControlDialog::payAmounts.clear();
    CoinControlDialog::payAmounts.append(amount);
    CoinControlDialog::updateLabels(window->walletModel, 0, this);
}

void ShadowBridge::updateCoinControlLabels(unsigned int &quantity, int64_t &amount, int64_t &fee, int64_t &afterfee, unsigned int &bytes, QString &priority, QString low, int64_t &change)
{
    emitCoinControlUpdate(quantity, amount, fee, afterfee, bytes, priority, low, change);
}

QVariantMap ShadowBridge::listAnonOutputs()
{
    QVariantMap anonOutputs;
    typedef std::map<int64_t, int> outputCount;

    outputCount mOwnedOutputCounts;
    outputCount mMatureOutputCounts;
    outputCount mSystemOutputCounts;

    if (pwalletMain->CountOwnedAnonOutputs(mOwnedOutputCounts,  false) != 0
     || pwalletMain->CountOwnedAnonOutputs(mMatureOutputCounts, true)  != 0)
    {
        printf("Error: CountOwnedAnonOutputs failed.\n");
        return anonOutputs;
    };

    for (std::map<int64_t, CAnonOutputCount>::iterator mi(mapAnonOutputStats.begin()); mi != mapAnonOutputStats.end(); mi++)
        mSystemOutputCounts[mi->first] = 0;

    if (pwalletMain->CountAnonOutputs(mSystemOutputCounts, true) != 0)
    {
        printf("Error: CountAnonOutputs failed.\n");
        return anonOutputs;
    };

    for (std::map<int64_t, CAnonOutputCount>::iterator mi(mapAnonOutputStats.begin()); mi != mapAnonOutputStats.end(); mi++)
    {
        CAnonOutputCount* aoc = &mi->second;
        QVariantMap anonOutput;

        int nDepth = aoc->nLeastDepth == 0 ? 0 : nBestHeight - aoc->nLeastDepth;

        anonOutput.insert("owned_mature",   mMatureOutputCounts[aoc->nValue]);
        anonOutput.insert("owned_outputs",  mOwnedOutputCounts [aoc->nValue]);
        anonOutput.insert("system_mature",  mSystemOutputCounts[aoc->nValue]);
        anonOutput.insert("system_outputs", aoc->nExists);
        anonOutput.insert("system_spends",  aoc->nSpends);
        anonOutput.insert("least_depth",    nDepth);
        anonOutput.insert("value_s",        BitcoinUnits::format(window->clientModel->getOptionsModel()->getDisplayUnit(), aoc->nValue));

        anonOutputs.insert(QString::number(aoc->nValue), anonOutput);
    };

    return anonOutputs;
};

void ShadowBridge::populateTransactionTable()
{
    if(transactionModel->thread() == thread())
    {
        transactionModel->init(window->clientModel, window->walletModel->getTransactionTableModel());
        connect(transactionModel, SIGNAL(emitTransactions(QVariantList)), SIGNAL(emitTransactions(QVariantList)), Qt::QueuedConnection);
        transactionModel->moveToThread(async);
    }

    transactionModel->populatePage();
}

void ShadowBridge::updateTransactions(QModelIndex topLeft, QModelIndex bottomRight)
{
    // Updated transactions...
    if(topLeft.column() == TransactionTableModel::Status)
        transactionModel->populateRows(topLeft.row(), bottomRight.row());
}

void ShadowBridge::insertTransactions(const QModelIndex & parent, int start, int end)
{
    // New Transactions...
    transactionModel->populateRows(start, end);
}

QString ShadowBridge::transactionDetails(QString txid)
{
    return window->walletModel->getTransactionTableModel()->index(window->walletModel->getTransactionTableModel()->lookupTransaction(txid), 0).data(TransactionTableModel::LongDescriptionRole).toString();
}

// Addresses
void ShadowBridge::populateAddressTable()
{
    if(addressModel->thread() == thread())
    {
        addressModel->atm = window->walletModel->getAddressTableModel();

        connect(addressModel, SIGNAL(emitAddresses(QVariantList)), SIGNAL(emitAddresses(QVariantList)), Qt::QueuedConnection);
        addressModel->moveToThread(async);
    }

    addressModel->populateAddressTable();
}

void ShadowBridge::updateAddresses(QModelIndex topLeft, QModelIndex bottomRight)
{
    addressModel->poplateRows(topLeft.row(), bottomRight.row());
}

void ShadowBridge::insertAddresses(const QModelIndex & parent, int start, int end)
{
    if(window->clientModel->inInitialBlockDownload()||addressModel->isRunning())
        return;

    addressModel->poplateRows(start, end);
}

QString ShadowBridge::newAddress(bool own)
{
    EditAddressDialog dlg(
            own ?
            EditAddressDialog::NewReceivingAddress :
            EditAddressDialog::NewSendingAddress);

    dlg.setModel(addressModel->atm);

    if(dlg.exec())
        return dlg.getAddress();

    return "";
}

QString ShadowBridge::getAddressLabel(QString address)
{
    return addressModel->atm->labelForAddress(address);
}

void ShadowBridge::updateAddressLabel(QString address, QString label)
{
    addressModel->atm->setData(addressModel->atm->index(addressModel->atm->lookupAddress(address), addressModel->atm->Label), QVariant(label), Qt::EditRole);
}

bool ShadowBridge::validateAddress(QString address)
{
    return window->walletModel->validateAddress(address);
}

bool ShadowBridge::deleteAddress(QString address)
{
    return addressModel->atm->removeRow(addressModel->atm->lookupAddress(address));
}

// Messages
void ShadowBridge::appendMessages(QString messages, bool reset)
{
    emitMessages("[" + messages + "]", reset);
}

void ShadowBridge::appendMessage(int row) {
    emitMessage(window->messageModel->index(row, MessageModel::Key)             .data().toString(),
                window->messageModel->index(row, MessageModel::Type)            .data().toString(),
                window->messageModel->index(row, MessageModel::SentDateTime)    .data().toDateTime().toTime_t(),
                window->messageModel->index(row, MessageModel::ReceivedDateTime).data().toDateTime().toTime_t(),
                window->messageModel->index(row, MessageModel::Label)           .data(MessageModel::LabelRole).toString(),
                window->messageModel->index(row, MessageModel::Label)           .data().toString().replace("\"","\\\"").replace("\\", "\\\\").replace("/", "\\/"),
                window->messageModel->index(row, MessageModel::ToAddress)       .data().toString(),
                window->messageModel->index(row, MessageModel::FromAddress)     .data().toString(),
                window->messageModel->index(row, MessageModel::Read)            .data().toBool(),
                window->messageModel->index(row, MessageModel::Message)         .data().toString().toHtmlEscaped());
}

void ShadowBridge::populateMessageTable()
{
    thMessage->mtm = window->messageModel;

    connect(thMessage, SIGNAL(emitMessages(QString, bool)), SLOT(appendMessages(QString, bool)));
    thMessage->start();
}

void ShadowBridge::insertMessages(const QModelIndex & parent, int start, int end)
{
    while(start <= end)
    {
        appendMessage(start++);
        qApp->processEvents();
    }
}

bool ShadowBridge::deleteMessage(QString key)
{
    return window->messageModel->removeRow(thMessage->mtm->lookupMessage(key));
}

bool ShadowBridge::markMessageAsRead(QString key)
{
    return window->messageModel->markMessageAsRead(key);
}

QString ShadowBridge::getPubKey(QString address, QString label)
{
    if(!label.isEmpty())
        updateAddressLabel(address, label);

    return addressModel->atm->pubkeyForAddress(address);;
}

bool ShadowBridge::setPubKey(QString address, QString pubkey)
{
    std::string sendTo = address.toStdString();
    std::string pbkey  = pubkey.toStdString();

    return SecureMsgAddAddress(sendTo, pbkey) == 0;
}

bool ShadowBridge::sendMessage(const QString &address, const QString &message, const QString &from)
{
    WalletModel::UnlockContext ctx(window->walletModel->requestUnlock());

    // Unlock wallet was cancelled
    if(!ctx.isValid())
        return false;

    MessageModel::StatusCode sendstatus = thMessage->mtm->sendMessage(address, message, from);

    switch(sendstatus)
    {
    case MessageModel::InvalidAddress:
        QMessageBox::warning(window, tr("Send Message"),
            tr("The recipient address is not valid, please recheck."),
            QMessageBox::Ok, QMessageBox::Ok);
        return false;
    case MessageModel::InvalidMessage:
        QMessageBox::warning(window, tr("Send Message"),
            tr("The message can't be empty."),
            QMessageBox::Ok, QMessageBox::Ok);
        return false;
    case MessageModel::DuplicateAddress:
        QMessageBox::warning(window, tr("Send Message"),
            tr("Duplicate address found, can only send to each address once per send operation."),
            QMessageBox::Ok, QMessageBox::Ok);
        return false;
    case MessageModel::MessageCreationFailed:
        QMessageBox::warning(window, tr("Send Message"),
            tr("Error: Message creation failed."),
            QMessageBox::Ok, QMessageBox::Ok);
        return false;
    case MessageModel::MessageCommitFailed:
        QMessageBox::warning(window, tr("Send Message"),
            tr("Error: The message was rejected."),
            QMessageBox::Ok, QMessageBox::Ok);
        return false;
    case MessageModel::Aborted:             // User aborted, nothing to do
        return false;
    case MessageModel::FailedErrorShown:    // Send failed, error message was displayed
        return false;
    case MessageModel::OK:
        break;
    }

    return true;
}


void ShadowBridge::connectSignals()
{
    connect(transactionModel->getModel(), SIGNAL(dataChanged(QModelIndex,QModelIndex)), SLOT(updateTransactions(QModelIndex,QModelIndex)));
    connect(transactionModel->getModel(), SIGNAL(rowsInserted(QModelIndex,int,int)),    SLOT(insertTransactions(QModelIndex,int,int)));

    connect(addressModel->atm,            SIGNAL(dataChanged(QModelIndex,QModelIndex)), SLOT(updateAddresses(QModelIndex,QModelIndex)));
    connect(addressModel->atm,            SIGNAL(rowsInserted(QModelIndex,int,int)),    SLOT(insertAddresses(QModelIndex,int,int)));

    connect(thMessage->mtm, SIGNAL(rowsInserted(QModelIndex,int,int)),    SLOT(insertMessages(QModelIndex,int,int)));
    connect(thMessage->mtm, SIGNAL(modelReset()),                         SLOT(populateMessageTable()));
}

QVariantMap ShadowBridge::userAction(QVariantMap action)
{
    QVariantMap::iterator it(action.begin());

    QString key(it.key());
    bool fOK;
    key.toInt(&fOK);

    if(fOK)
        key = it.value().toString();

    if(key == "backupWallet")
        window->backupWallet();
    if(key == "verifyMessage")
        window->gotoVerifyMessageTab(it.value().toString());
    if(key == "signMessage")
        window->gotoSignMessageTab  (it.value().toString());
    if(key == "close")
        window->close();
    if(key == "encryptWallet")
        window->encryptWallet(true);
    if(key == "changePassphrase")
        window->changePassphrase();
    if(key == "toggleLock")
        window->toggleLock();
    if(key == "developerConsole")
        window->webView->page()->triggerAction(QWebPage::InspectElement);
    if(key == "aboutClicked")
        window->aboutClicked();
    if(key == "aboutQtClicked")
        window->aboutQtAction->trigger();
    if(key == "debugClicked")
        window->rpcConsole->show();
    if(key == "clearRecipients")
        clearRecipients();
    if(key == "optionsChanged")
    {
        OptionsModel * optionsModel(window->clientModel->getOptionsModel());
        QVariantMap value(it.value().toMap());

        for(int option = 0;option < optionsModel->rowCount(); option++)
            if(value.contains(optionsModel->optionIDName(option)))
                optionsModel->setData(optionsModel->index(option), value.value(optionsModel->optionIDName(option)));

        populateOptions();
    }

    return QVariantMap();
}
