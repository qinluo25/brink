#include "tcp_socket.h"
#include <boost/bind.hpp>

BrinK::tcp::socket::socket() :socket_(nullptr), timer_(nullptr), strand_(nullptr), recv_buff_(nullptr), param_(nullptr)
{

}

BrinK::tcp::socket::~socket()
{

}

boost::asio::ip::tcp::socket& BrinK::tcp::socket::raw_socket()
{
    return *socket_;
}

void BrinK::tcp::socket::async_read(const size_t& expect_size,
    const client_handler_t& recv_handler,
    const unsigned __int64& time_out_milliseconds,
    const client_handler_t& timeout_handler)
{
    std::unique_lock < std::mutex > lock(mutex_);

    if (!avalible_)
        return;

    set_timeout(time_out_milliseconds, timeout_handler);
    
    socket_->async_read_some(
        recv_buff_->prepare(expect_size),
        strand_->wrap(boost::bind(&socket::handle_read,
        shared_from_this(),
        boost::asio::placeholders::error,
        boost::asio::placeholders::bytes_transferred,
        recv_handler,
        expect_size,
        recv_buff_,
        socket_
        )));
}

void BrinK::tcp::socket::handle_read(const boost::system::error_code& error,
    size_t bytes_transferred,
    const client_handler_t& handler,
    const size_t& expect_size,
    streambuf_sptr_t sbuff,
    tcp_socket_sptr_t socket)
{
    // 提交收到的数据（也许不完整）
    sbuff->commit(bytes_transferred);

    if ((error) || (sbuff->size() >= expect_size))
    {
        // 如果出错或完成，进行回调并清空数据
        handler(shared_from_this(), error, bytes_transferred, BrinK::utils::streambuf_to_string(sbuff));
        sbuff->consume(sbuff->size());
    }
    else
    {
        // 如果预期数据不完整，继续接收
        socket->async_read_some(
            sbuff->prepare(expect_size - bytes_transferred),
            strand_->wrap(boost::bind(&tcp::socket::handle_read,
            shared_from_this(),
            boost::asio::placeholders::error,
            boost::asio::placeholders::bytes_transferred,
            handler,
            expect_size,
            sbuff,
            socket
            )));
    }
}

void BrinK::tcp::socket::async_write(const std::string& buff, const client_handler_t& handler)
{
    std::unique_lock < std::mutex > lock(mutex_);

    if (!avalible_)
        return;

    std::unique_lock < std::mutex > lock_buff(send_buff_mutex_);

    send_buff_list_.emplace_back(std::make_shared< boost::asio::streambuf >());
    std::ostream oa(send_buff_list_.front().get());

    oa << buff;

    socket_->async_write_some(
        send_buff_list_.front()->data(),
        strand_->wrap(boost::bind(&tcp::socket::handle_write,
        shared_from_this(),
        boost::asio::placeholders::error,
        boost::asio::placeholders::bytes_transferred,
        handler,
        send_buff_list_.front()->size(),
        send_buff_list_.front(),
        socket_,
        buff
        )));
}

void BrinK::tcp::socket::handle_write(const boost::system::error_code& error,
    size_t bytes_transferred,
    const client_handler_t& handler,
    const size_t& expect_size,
    streambuf_sptr_t sbuff,
    tcp_socket_sptr_t socket,
    const std::string& buff)
{
    if ((error) || (bytes_transferred >= expect_size))
    {
        {
            std::unique_lock < std::mutex > lock(send_buff_mutex_);
            send_buff_list_.remove(sbuff);
        }

        handler(shared_from_this(), error, bytes_transferred, buff);
    }
    else
    {
        sbuff->consume(bytes_transferred);

        socket->async_write_some(
            sbuff->data(),
            strand_->wrap(boost::bind(&tcp::socket::handle_write,
            shared_from_this(),
            boost::asio::placeholders::error,
            boost::asio::placeholders::bytes_transferred,
            handler,
            sbuff->size(),
            sbuff,
            socket,
            buff
            )));
    }
}

void BrinK::tcp::socket::reset(boost::asio::io_service& io_service)
{
    std::unique_lock < std::mutex > lock(mutex_);

    std::unique_lock < std::mutex > lock_buff(send_buff_mutex_);
    send_buff_list_.clear();

    strand_ = std::make_shared < boost::asio::strand > (io_service);
    recv_buff_ = std::make_shared < boost::asio::streambuf > ();
    timer_ = std::make_shared < boost::asio::deadline_timer > (io_service);
    socket_ = std::make_shared < boost::asio::ip::tcp::socket > (io_service);
    param_ = std::make_shared < BrinK::param > ();

    timeout_count_ = 0;

    avalible_ = true;
}

void BrinK::tcp::socket::closesocket()
{
    std::unique_lock < std::mutex > lock(mutex_);

    if (!avalible_)
        return;

    boost::system::error_code ec;
    socket_->shutdown(boost::asio::socket_base::shutdown_both, ec);
    socket_->close(ec);
}

void BrinK::tcp::socket::free()
{
    std::unique_lock < std::mutex > lock(mutex_);

    std::unique_lock < std::mutex > lock_buff(send_buff_mutex_);
    send_buff_list_.clear();

    avalible_ = false;
    socket_.reset();
    strand_.reset();
    recv_buff_.reset();
    param_.reset();

    boost::system::error_code ec;
    timer_->cancel(ec);
    timer_.reset();
}

void BrinK::tcp::socket::accept()
{
    std::unique_lock < std::mutex > lock(mutex_);

    if (!avalible_)
        return;
    
    boost::system::error_code ec;
    socket_->set_option(boost::asio::ip::tcp::acceptor::linger(true, 0), ec);
    socket_->set_option(boost::asio::socket_base::keep_alive(true), ec);
    socket_->set_option(boost::asio::ip::tcp::no_delay(true), ec);

    param_->unique_id = socket_->remote_endpoint(ec).address().to_string(ec) +
        ":" +
        BrinK::utils::to_string < unsigned short > (socket_->remote_endpoint(ec).port());
}

void BrinK::tcp::socket::set_timeout(const unsigned __int64& milliseconds, const client_handler_t& handler)
{
    timer_->expires_from_now(boost::posix_time::milliseconds(milliseconds));

    timer_->async_wait(
        boost::bind(&socket::handle_timeout,
        shared_from_this(),
        boost::asio::placeholders::error,
        timer_,
        milliseconds,
        handler
        ));
}

void BrinK::tcp::socket::handle_timeout(const boost::system::error_code& error,
    timer_sptr_t timer,
    const unsigned __int64& time_out_milliseconds,
    const client_handler_t& handler)
{
    if (error)
        return;

    if (!avalible_)
        return;

    if (timer->expires_at() <= boost::asio::deadline_timer::traits_type::now())
    {
        ++timeout_count_;
        handler(shared_from_this(), boost::asio::error::timed_out, timeout_count_, "");
    }

    timer->expires_from_now(boost::posix_time::milliseconds(time_out_milliseconds));

    timer->async_wait(
        boost::bind(&socket::handle_timeout,
        shared_from_this(),
        boost::asio::placeholders::error,
        timer,
        time_out_milliseconds,
        handler
        ));
}

void BrinK::tcp::socket::get_param(const std::function < void(param_sptr_t p) >& handler)
{
    std::unique_lock < std::mutex > lock_param(param_mutex_);
    handler(param_);
}




