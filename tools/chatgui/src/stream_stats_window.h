/* Окно статистики автора: две гистограммы и шесть графиков выбранного узла; только GUI-поток.
 * История собирается ядром независимо от окна; таймер UI работает только при видимости. */
#pragma once
#include "../transport/stream_bridge.h"
#include "../../../lib/debug_config.h"
#include <QWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QLabel>
#include <QTableWidget>
#include <QStyledItemDelegate>
#include <QHeaderView>
#include <QPainter>
#include <QPainterPath>
#include <QShowEvent>
#include <QHideEvent>
#include <algorithm>
#include <cmath>
#include <utility>

/* Скорость в B/s с десятичными приставками. В таблице исходное число сохраняется для сортировки. */
inline QString streamStatsRate(uint64_t bytesPerSecond) {
    if (bytesPerSecond < 1000) return QString::number(bytesPerSecond);
    const char prefixes[] = "KMGTPE";
    double scaled = bytesPerSecond / 1000.0;
    unsigned unit = 0;
    while (scaled >= 999.95 && unit + 1 < sizeof(prefixes) - 1) { scaled /= 1000; ++unit; }
    QString value = QString::number(scaled, 'f', 1);
    if (value.endsWith(QStringLiteral(".0"))) value.chop(2);
    return value + QLatin1Char(prefixes[unit]);
}

class StreamStatsRateDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QString displayText(const QVariant& value, const QLocale&) const override { return streamStatsRate(value.toULongLong()); }
};

inline QByteArray nodeKey(const stream_address& address) {
    return QByteArray(reinterpret_cast<const char*>(address.ed), 32);
}

class StreamStatsCanvas : public QWidget {
public:
    std::vector<stream_stat_node> nodes; // Снимок истории всех узлов для гистограмм и графиков.
    uint64_t now = 0;
    QByteArray selected; // Часы снимка, мс, и ID выбранного в таблице узла.
    explicit StreamStatsCanvas(QWidget* parent) : QWidget(parent) { setMinimumSize(1000, 540); }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this); p.fillRect(rect(), palette().window()); p.setRenderHint(QPainter::Antialiasing);
        const QColor ink = palette().text().color(), blue(52,152,219);
        const double gap = 16, columnWidth = (width() - gap) / 2.0;
        auto histogram = [&](double x, bool pauses) {
            const double limits[] = {100,250,500,1000,2000,5000,INFINITY};
            const double pauseLimits[] = {0,10,25,50,75,100,INFINITY};
            const QStringList labels = pauses ? QStringList{"0%",">0–10%","10–25%","25–50%","50–75%","75–100%"}
                : QStringList{"0–100","100–250","250–500","500–1000","1000–2000","2000–5000",">5000"};
            unsigned bins[7] = {0}, missing = 0;
            for (const auto& node : nodes) {
                if (!node.count) continue;
                const auto& s = node.samples[node.count-1];
                if (now - s.at_ms >= STREAM_STATS_STALE_MS) continue;
                if ((!pauses && s.v[ST_LAG] == UINT64_MAX) || (pauses && !s.v[ST_AUDIO_OBSERVED])) { missing++; continue; }
                double value = pauses ? 100.0 * s.v[ST_PAUSE_MS] / s.v[ST_AUDIO_OBSERVED] : s.v[ST_LAG];
                unsigned bin = 0; while (bin + 1 < unsigned(labels.size()) && value > (pauses ? pauseLimits[bin] : limits[bin])) bin++;
                bins[bin]++;
            }
            p.setPen(ink); p.drawText(QPointF(x+8,18), (pauses ? QStringLiteral("Звук: доля тишины из-за нехватки данных")
                : QStringLiteral("Отставание от автора, мс")) + QStringLiteral(" · нет данных: %1").arg(missing));
            unsigned maximum = 1; for (auto v : bins) maximum = std::max(maximum, v);
            const double step = (columnWidth-16)/labels.size();
            p.save(); QFont font = p.font(); font.setPointSizeF(std::max(7.0, font.pointSizeF()-1)); p.setFont(font);
            for (int i=0; i<labels.size(); i++) {
                const double h = 52.0 * bins[i]/maximum, left = x+8+i*step;
                p.fillRect(QRectF(left+4,94-h,step-8,h),blue);
                p.drawText(QRectF(left,74-h,step,20),Qt::AlignCenter,QString::number(bins[i]));
                p.drawText(QRectF(left,98,step,20),Qt::AlignCenter,labels[i]);
            }
            p.setPen(palette().mid().color()); p.drawLine(QPointF(x+8,94),QPointF(x+columnWidth-8,94)); p.restore();
        };
        histogram(0,false); histogram(columnWidth+gap,true);
        p.setPen(ink); p.drawText(8,145,QStringLiteral("Выбранный узел · последние 5 минут · 100% CPU = одно ядро"));
        const stream_stat_node* node = nullptr;
        for (const auto& n : nodes) if (nodeKey(n.node) == selected) node = &n;
        const std::pair<const char*,unsigned> fields[] = {{"Задержка, мс",ST_LAG},{"Макс. пауза звука, мс",ST_PAUSE_MAX},
            {"TX, B/s",ST_TX},{"RX, B/s",ST_RX},{"CPU процесса, %",ST_CPU},{"CPU uasync, %",ST_UASYNC}};
        const double rowHeight = (height()-158-gap*2)/3.0;
        for (unsigned chart=0;chart<6;chart++) {
            const unsigned field = fields[chart].second;
            const double x = (chart%2)*(columnWidth+gap), y = 158+(chart/2)*(rowHeight+gap);
            const double factor = field == ST_CPU || field == ST_UASYNC ? 100.0 : 1.0;
            uint64_t peak = 0; bool available = false;
            if (node) for (unsigned i=0;i<node->count;i++) {
                const auto& s = node->samples[i];
                if (now-s.at_ms>STREAM_STATS_HISTORY_MS || s.v[field]==UINT64_MAX) continue;
                peak=std::max(peak,s.v[field]); available=true;
            }
            const double maximum = std::max(1.0, peak / factor);
            p.setPen(ink); p.drawText(QPointF(x+8,y+15),QString::fromUtf8(fields[chart].first));
            const QRectF box(x+62,y+26,columnWidth-74,rowHeight-48);
            const QString topLabel = field == ST_TX || field == ST_RX ? streamStatsRate(peak ? peak : 1) : QString::number(maximum,'g',4);
            p.drawText(QRectF(x,y+18,56,20),Qt::AlignRight|Qt::AlignVCenter,topLabel);
            p.drawText(QRectF(x,box.bottom()-10,56,20),Qt::AlignRight|Qt::AlignVCenter,QStringLiteral("0"));
            p.setPen(palette().mid().color()); p.drawRect(box);
            p.setPen(QPen(palette().mid().color(),1,Qt::DotLine));
            p.drawLine(QPointF(box.left(),box.center().y()),QPointF(box.right(),box.center().y()));
            p.setPen(ink);
            p.drawText(QRectF(box.left(),box.bottom()+4,100,18),Qt::AlignLeft,QStringLiteral("−5 минут"));
            p.drawText(QRectF(box.right()-100,box.bottom()+4,100,18),Qt::AlignRight,QStringLiteral("сейчас"));
            if (!node || !available) {
                p.drawText(box,Qt::AlignCenter,node ? QStringLiteral("Нет данных") : QStringLiteral("Выберите узел в таблице"));
                continue;
            }
            p.save(); p.setClipRect(box.adjusted(-2,-2,2,2));
            QPainterPath path; bool started=false; uint64_t previous=0, sequence=0, generation=0;
            for (unsigned i=0;i<node->count;i++) {
                const auto& s=node->samples[i];
                if (now-s.at_ms>STREAM_STATS_HISTORY_MS || s.v[field]==UINT64_MAX) { started=false; continue; }
                QPointF point(box.right()-(now-s.at_ms)*box.width()/STREAM_STATS_HISTORY_MS,
                    box.bottom()-s.v[field]/factor*box.height()/maximum);
                if (!started || s.at_ms-previous>STREAM_STATS_STALE_MS || s.sequence!=sequence+1 || s.generation!=generation)
                    path.moveTo(point);
                else path.lineTo(point);
                p.fillRect(QRectF(point.x()-2,point.y()-2,4,4),blue);
                started=true; previous=s.at_ms; sequence=s.sequence; generation=s.generation;
            }
            p.setPen(QPen(blue,2)); p.drawPath(path); p.setPen(ink);
            for (unsigned i=1;i<node->count;i++) {
                const auto& s=node->samples[i];
                if (s.v[ST_NAT]==node->samples[i-1].v[ST_NAT] || now-s.at_ms>STREAM_STATS_HISTORY_MS) continue;
                const double at=box.right()-(now-s.at_ms)*box.width()/STREAM_STATS_HISTORY_MS;
                p.drawLine(QPointF(at,box.top()),QPointF(at,box.bottom()));
                const char* names[]={"?","IP","EIM","strict"};
                p.drawText(QPointF(at+3,box.top()+16),QString::fromLatin1(names[s.v[ST_NAT]]));
            }
            p.restore();
        }
    }
};

class StreamStatsWindow : public QWidget {
    QString group;
    uint64_t epoch, now=0;
    QTimer timer;
    QLabel* summary;
    QLabel* details;
    QTableWidget* table;
    StreamStatsCanvas* canvas;
public:
    StreamStatsWindow(const QString& g, uint64_t e, QWidget* parent) : QWidget(parent,Qt::Window),group(g),epoch(e) {
        setObjectName("streamStatisticsWindow");setWindowTitle(QStringLiteral("Статистика трансляции"));resize(1180,900);
        auto* layout=new QVBoxLayout(this);summary=new QLabel(this);summary->setWordWrap(true);layout->addWidget(summary);
        table=new QTableWidget(0,13,this);table->setObjectName("streamStatisticsNodes");
        table->setHorizontalHeaderLabels({"Узел","Роль","NAT","Состояние","Возраст, с","Задержка, мс","Макс. пауза звука, мс",
            "RX, B/s","TX, B/s","Вход / выход","Очередь, B","CPU, %","uasync, %"});
        table->setSelectionBehavior(QAbstractItemView::SelectRows);table->setSelectionMode(QAbstractItemView::SingleSelection);
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);table->setSortingEnabled(true);
        table->setItemDelegateForColumn(7,new StreamStatsRateDelegate(table));
        table->setItemDelegateForColumn(8,new StreamStatsRateDelegate(table));
        table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);layout->addWidget(table,1);
        details=new QLabel(this);details->setWordWrap(true);layout->addWidget(details);
        canvas=new StreamStatsCanvas(this);layout->addWidget(canvas,2);
        connect(table,&QTableWidget::itemSelectionChanged,this,[this]{
            if(table->currentRow()>=0 && table->item(table->currentRow(),0)) {
                canvas->selected=table->item(table->currentRow(),0)->data(Qt::UserRole).toByteArray();canvas->update();
                DEBUG_DEBUG(DEBUG_CATEGORY_STREAM, "desktop statistics selected node=%s charts=lag,pause,tx,rx,cpu,uasync",
                    canvas->selected.toHex().constData());
            }
        });
        timer.setInterval(1000);connect(&timer,&QTimer::timeout,this,[this]{refresh();});
        DEBUG_DEBUG(DEBUG_CATEGORY_STREAM, "desktop statistics layout histograms=2x1 charts=2x3 group=%s epoch=%llx",
            qPrintable(group), (unsigned long long)epoch);
    }
    void refresh() {
        std::vector<stream_stat_node> nodes;
        if(!gui_stream_statistics(group,epoch,nodes,now))return;
        canvas->nodes=nodes;canvas->now=now;
        unsigned viewers=0,relays=0,stale=0,paused=0,nats[4]={0}; uint64_t rx=0,tx=0,queued=0,events=0;
        std::vector<uint64_t> lags,cpus,loopCpus;
        table->blockSignals(true);table->setSortingEnabled(false);table->setRowCount(int(nodes.size()));int selectedRow=-1;
        for(size_t row=0;row<nodes.size();row++) {
            const auto& node=nodes[row];if(!node.count)continue;const auto& s=node.samples[node.count-1];
            bool old=now-s.at_ms>=STREAM_STATS_STALE_MS;
            if(old)stale++;else {
                if(s.v[ST_RELAY])relays++;else viewers++;nats[s.v[ST_NAT]]++;paused+=s.v[ST_PAUSED]!=0;
                rx+=s.v[ST_RX];tx+=s.v[ST_TX];queued+=s.v[ST_QUEUE];events+=s.v[ST_PAUSES];
                if(s.v[ST_UASYNC]!=UINT64_MAX)loopCpus.push_back(s.v[ST_UASYNC]);
                if(s.v[ST_LAG]!=UINT64_MAX)lags.push_back(s.v[ST_LAG]);if(s.v[ST_CPU]!=UINT64_MAX)cpus.push_back(s.v[ST_CPU]);
            }
            auto number=[](uint64_t v,double scale=1.0){return v==UINT64_MAX?QStringLiteral("нет данных"):QString::number(v/scale,'f',scale==1?0:1);};
            const char* types[]={"не определён","прямой IP","EIM","strict"};
            QVariantList cells={node.name[0]?QString::fromUtf8(node.name):QString::fromLatin1(nodeKey(node.node).toHex()),s.v[ST_RELAY]?"relay":"зритель",
                types[s.v[ST_NAT]],old?"нет свежего отчёта":s.v[ST_FINISHED]?"завершён":s.v[ST_PAUSED]?"пауза":!s.v[ST_OBSERVED]?"запуск":"приём",
                number(now-s.at_ms,1000),number(s.v[ST_LAG]),number(s.v[ST_PAUSE_MAX]),qulonglong(s.v[ST_RX]),qulonglong(s.v[ST_TX]),
                number(s.v[ST_IN])+" / "+number(s.v[ST_OUT]),number(s.v[ST_QUEUE]),number(s.v[ST_CPU],100),number(s.v[ST_UASYNC],100)};
            for(int col=0;col<cells.size();col++) {
                auto* item=new QTableWidgetItem;item->setData(Qt::DisplayRole,cells[col]);table->setItem(int(row),col,item);
                if(col==0)item->setData(Qt::UserRole,nodeKey(node.node));
                item->setToolTip(QStringLiteral("ID: %1 · поставщики: %2 · получатели: %3 · части: %4/%5 · возраст очереди: %6 мс · пропуски: %7 · новые паузы: %8")
                    .arg(QString::fromLatin1(nodeKey(node.node).toHex())).arg(s.v[ST_PROVIDERS]).arg(s.v[ST_RECEIVERS]).arg(s.v[ST_PARTS]).arg(s.v[ST_REQUIRED])
                    .arg(s.v[ST_QUEUE_AGE]).arg(s.v[ST_GAPS]).arg(s.v[ST_PAUSES]));
            }
            if(nodeKey(node.node)==canvas->selected)selectedRow=int(row);
        }
        table->setSortingEnabled(true);
        if(selectedRow>=0)for(int row=0;row<table->rowCount();row++)if(table->item(row,0)->data(Qt::UserRole).toByteArray()==canvas->selected)table->selectRow(row);
        table->blockSignals(false);
        for (const auto& node : nodes) if (nodeKey(node.node)==canvas->selected && node.count) {
            const auto& s=node.samples[node.count-1];
            details->setText(QStringLiteral("Узел %1 · интервал %2 мс · новые паузы звука: %3 · суммарно: %4 мс · прогресс: %5 мс назад · фаза: %6\n"
                "Очередь: %7 частей / %8 B, возраст %9 мс · удалено %10 B · пропуски %11 · отвергнуто %12 · дубликаты %13\n"
                "Смены поставщиков: %14 · переходы на KEY: %15 · полезная скорость %16 B/s · изменение очереди %17 B")
                .arg(QString::fromLatin1(nodeKey(node.node).toHex())).arg(s.v[ST_DT]).arg(s.v[ST_PAUSES]).arg(s.v[ST_PAUSE_MS])
                .arg(s.v[ST_AGE]==UINT64_MAX?QStringLiteral("нет данных"):QString::number(s.v[ST_AGE])).arg(s.v[ST_PHASE])
                .arg(s.v[ST_QUEUE_PARTS]).arg(s.v[ST_QUEUE]).arg(s.v[ST_QUEUE_AGE]).arg(s.v[ST_REMOVED]).arg(s.v[ST_GAPS])
                .arg(s.v[ST_REJECTED]).arg(s.v[ST_DUPLICATES]).arg(s.v[ST_MIGRATIONS]).arg(s.v[ST_JUMPS])
                .arg(streamStatsRate(s.v[ST_USEFUL])).arg(static_cast<qlonglong>(s.v[ST_QUEUE_DELTA])));
        }
        std::sort(lags.begin(),lags.end());std::sort(cpus.begin(),cpus.end());
        QString delay=lags.empty()?"нет данных":QStringLiteral("медиана %1 / P95 %2 / max %3 мс (N=%4)")
            .arg(lags[(lags.size()-1)/2] + (lags[lags.size()/2]-lags[(lags.size()-1)/2])/2).arg(lags[(lags.size()*95-1)/100]).arg(lags.back()).arg(lags.size());
        double cpuSum=0;for(auto v:cpus)cpuSum+=v;
        double loopSum=0,loopMax=0;for(auto v:loopCpus){loopSum+=v;loopMax=std::max(loopMax,double(v));}
        summary->setText(QStringLiteral("Зрители: %1 · relay: %2 · устаревшие: %3 · с паузой звука: %4 · новые паузы: %5\n"
            "Прямой IP: %6 · EIM: %7 · strict: %8 · не определён: %9\n"
            "RX: %10 B/s · TX: %11 B/s · очередь: %12 B · задержка: %13\nCPU процесса: среднее %14%% / max %15%% (N=%16)")
            .arg(viewers).arg(relays).arg(stale).arg(paused).arg(events).arg(nats[1]).arg(nats[2]).arg(nats[3]).arg(nats[0])
            .arg(streamStatsRate(rx)).arg(streamStatsRate(tx)).arg(queued).arg(delay)
            .arg(cpus.empty()?QStringLiteral("нет данных"):QString::number(cpuSum/cpus.size()/100,'f',1))
            .arg(cpus.empty()?QStringLiteral("нет данных"):QString::number(cpus.back()/100.0,'f',1)).arg(cpus.size()) + QStringLiteral(" · uasync: среднее %1% / max %2% (N=%3)")
            .arg(loopCpus.empty()?QStringLiteral("нет данных"):QString::number(loopSum/loopCpus.size()/100,'f',1))
            .arg(loopCpus.empty()?QStringLiteral("нет данных"):QString::number(loopMax/100,'f',1)).arg(loopCpus.size()));canvas->update();
    }
protected:
    void showEvent(QShowEvent* e) override {QWidget::showEvent(e);refresh();timer.start();}
    void hideEvent(QHideEvent* e) override {timer.stop();QWidget::hideEvent(e);}
};
