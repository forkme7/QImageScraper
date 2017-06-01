#include "image_downloader.hpp"

#include "global_constant.hpp"
#include "utility.hpp"

#include <qt_enhance/network/download_supervisor.hpp>
#include <qt_enhance/utility/qte_utility.hpp>

#include <QsLog.h>

#include <QFileInfo>
#include <QImageReader>

image_downloader::image_downloader(QObject *parent) :
    QObject(parent),
    download_state_{download_state::normal},
    downloader_{new qte::net::download_supervisor(this)}
{
    using namespace qte::net;

    connect(downloader_, &download_supervisor::error, this, &image_downloader::download_image_error);
    connect(downloader_, &download_supervisor::download_finished, this, &image_downloader::download_finished);
    connect(downloader_, &download_supervisor::download_progress, this, &image_downloader::download_progress);
}

void image_downloader::set_download_request(QStringList big_image_links, QStringList small_image_links,
                                            size_t max_download, QString const &save_at)
{
    big_img_links_.swap(big_image_links);
    small_img_links_.swap(small_image_links);
    QLOG_INFO()<<__func__<<":big img:"<<big_img_links_.size()<<",small img:"<<small_img_links_.size();
    statistic_.clear();
    img_links_map_.clear();
    save_at_ = save_at;
    download_state_ = download_state::normal;
    statistic_.total_download_ = std::min(static_cast<size_t>(big_img_links_.size()),
                                          max_download);
}

void image_downloader::download_finished(image_downloader::download_img_task task)
{
    QLOG_INFO()<<__func__<<":"<<task->get_unique_id()<<":"<<task->get_network_error_code();
    QLOG_INFO()<<__func__<<"save as:"<<task->get_save_as()<<",url:"<<task->get_url();
    auto it = img_links_map_.find(task->get_unique_id());
    if(it != std::end(img_links_map_)){
        auto img_info = it->second;
        img_links_map_.erase(it);
        if(task->get_is_timeout()){
            QLOG_INFO()<<__func__<<":"<<task->get_save_as()<<","<<task->get_url()<<": timeout";
            emit set_statusbar_msg(tr("Waiting reply from the server, please give some patient"));
            remove_file("time out issue:", task);
            download_image(std::move(img_info));
            return;
        }

        process_download_image(task, std::move(img_info));

        refresh_window();
    }else{
        increase_progress();
        remove_file("cannot find id in img_links_map, can remove file:", task);
        download_next_image();
    }
}

void image_downloader::download_image(image_downloader::img_links_map_value info)
{
    auto const choice = info.choice_;
    if(choice != link_choice::web_view){
        download_state_ = download_state::normal;
    }else{
        download_state_ = download_state::show_big_img_by_web_view;
    }

    QString const &img_link = choice == link_choice::big ?
                info.big_img_link_ : info.small_img_link_;
    QNetworkRequest const request = create_img_download_request(img_link);
    auto const unique_id = downloader_->append(request, save_at_,
                                               global_constant::network_reply_timeout());
    img_links_map_.emplace(unique_id, std::move(info));
    QTimer::singleShot(qrand() % 1000 + 500, [this, unique_id](){downloader_->start_download_task(unique_id);});
}

void image_downloader::download_image_error(download_img_task task, const QString &error_msg)
{
    QLOG_ERROR()<<__func__<<":"<<task->get_unique_id()<<":"<<error_msg;
}

void image_downloader::download_small_img(image_downloader::img_links_map_value img_info)
{
    if(img_info.choice_ == link_choice::web_view && img_info.big_img_link_ != img_info.small_img_link_ &&
            !img_info.small_img_link_.isEmpty()){
        img_info.choice_ = link_choice::small;
        img_info.retry_num_ = 0;
        download_image(std::move(img_info));
    }else{
        increase_progress();
        download_next_image();
    }
}

void image_downloader::download_web_view_img(img_links_map_value img_info)
{
    QLOG_INFO()<<"enter "<<__func__;
    if(img_info.choice_ == link_choice::big){
        QLOG_INFO()<<__func__<<" : download big img by web_view";
        download_state_ = download_state::show_big_img_by_web_view;
        //non scalable for multi download, but easier to implement
        img_info_ = std::move(img_info);
        img_info_.choice_ = link_choice::web_view;
        img_info.retry_num_ = 0;
        emit load_image(img_info_.big_img_link_);
    }else{
        increase_progress();
        download_next_image();
    }
}

QString image_downloader::get_valid_image_name(QString const &save_as, QString const &img_format)
{
    QFileInfo const file_info(save_as);
    QString const suffix = file_info.suffix() == "jpg" ? "jpeg" : file_info.suffix();

    bool const change_suffix = suffix != img_format;
    if(change_suffix){
        QString const new_name = file_info.absolutePath() + "/" +
                file_info.completeBaseName() + "." + img_format;

        return new_name;
    }else{
        return save_as;
    }
}

void image_downloader::increase_progress()
{
    ++statistic_.downloaded_file_;
    emit increment_progress();
}

void image_downloader::process_download_image(image_downloader::download_img_task task,
                                              image_downloader::img_links_map_value img_info)
{
    bool img_can_read = true;
    QByteArray format;
    {
        QImageReader img(task->get_save_as());
        img.setDecideFormatFromContent(true);
        img_can_read = img.canRead();
        format = img.format();
    }
    if(task->get_network_error_code() == QNetworkReply::NoError && img_can_read){
        QLOG_INFO()<<"can save image choice:"<<(int)img_info.choice_;
        QString const &valid_name = get_valid_image_name(task->get_save_as(), format);
        if(valid_name != task->get_save_as()){
            bool const can_rename_img = QFile::rename(task->get_save_as(), valid_name);
            QLOG_INFO()<<"QFile::rename, can rename image from:"<<task->get_save_as()<<", to:"<<
                         valid_name<<":"<<format<<":"<<can_rename_img;
        }
        increase_progress();
        if(img_info.choice_ == link_choice::big){
            ++statistic_.big_img_download_;
        }else{
            ++statistic_.small_img_download_;
        }
        img_info_.retry_num_ = 0;
        download_next_image();
    }else{
        if(task->get_network_error_code() != QNetworkReply::NoError &&
                img_info.retry_num_++ < global_constant::download_retry_limit()){
            download_image(std::move(img_info));
            return;
        }else{
            img_info.retry_num_ = 0;
        }
        QLOG_INFO()<<"cannot save image choice:"<<(int)img_info.choice_;
        remove_file("big image, can remove file:", task);
        if(img_info.choice_ == link_choice::big){
            download_web_view_img(std::move(img_info));
        }else if(img_info.choice_ == link_choice::web_view){
            download_small_img(std::move(img_info));
        }
    }
}

void image_downloader::remove_file(const QString &debug_msg, image_downloader::download_img_task task)
{
    bool const can_remove = QFile::remove(task->get_save_as());
    QLOG_INFO()<<__func__<<":"<<debug_msg<<task->get_save_as()<<":"<<can_remove;
}

void image_downloader::start_download(const QString &big_img_link, const QString &small_img_link)
{
    static size_t img_link_count = 0;
    QLOG_INFO()<<"found image link count:"<<img_link_count++;
    download_image({big_img_link, small_img_link, link_choice::big});
}

void image_downloader::download_next_image()
{
    QLOG_INFO()<<"enter : "<<__func__;
    if(!big_img_links_.empty()){
        QLOG_INFO()<<__func__<<":big img is not empty";
        auto const big_img = big_img_links_[0];
        auto const small_img = small_img_links_[0];
        if(statistic_.downloaded_file_ != statistic_.total_download_){
            QLOG_INFO()<<__func__<<":need to download more image";
            big_img_links_.pop_front();
            small_img_links_.pop_front();
            emit load_image(small_img);
            start_download(big_img, small_img);
        }else{
            QLOG_INFO()<<__func__<<":reach download target";
            big_img_links_.clear();
            small_img_links_.clear();
            img_links_map_.clear();
            refresh_window();
        }
    }
}

void image_downloader::download_web_view_img()
{
    download_web_view_img(std::move(img_info_));
}

image_downloader::download_state image_downloader::get_download_state() const
{
    return download_state_;
}

QNetworkAccessManager *image_downloader::get_network_manager() const
{
    return downloader_->get_network_manager();
}

image_downloader::download_statistic image_downloader::get_statistic() const
{
    return statistic_;
}

bool image_downloader::image_links_empty() const
{
    return big_img_links_.empty() && small_img_links_.empty();
}

void image_downloader::process_web_view_image(QImage img, QString const &url)
{
    if(!img.isNull()){
        QLOG_INFO()<<"process_load_url_done->image is not null";
        QString const file_name =
                qte::utils::unique_file_name(save_at_, QFileInfo(url).fileName());
        QLOG_INFO()<<"process_load_url_done->save image as:"<<file_name;
        if(img.save(get_valid_image_name(save_at_ + "/" + file_name, "jpg"))){
            increase_progress();
            download_next_image();
        }else{
            QLOG_INFO()<<"process_load_url_done->cannot save downloaded image:"
                      <<save_at_ + "/" + file_name;
            download_small_img(std::move(img_info_));
        }
    }else{
        QLOG_INFO()<<"process_load_url_done->cannot save image:"<<url;
        download_small_img(std::move(img_info_));
    }
}

void image_downloader::download_statistic::clear()
{
    big_img_download_ = 0;
    downloaded_file_ = 0;
    small_img_download_ = 0;
    total_download_ = 0;
}

size_t image_downloader::download_statistic::fail() const
{
    return total_download_ - success();
}

size_t image_downloader::download_statistic::success() const
{
    return big_img_download_ + small_img_download_;
}

image_downloader::img_links_map_value::img_links_map_value() :
    choice_{link_choice::big},
    retry_num_{0}
{

}

image_downloader::img_links_map_value::img_links_map_value(QString big_img_link, QString small_img_link,
                                                           image_downloader::link_choice choice, size_t retry_num) :
    big_img_link_(std::move(big_img_link)),
    small_img_link_(std::move(small_img_link)),
    choice_(choice),
    retry_num_(retry_num)
{

}