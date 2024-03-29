#include <eosio.token/eosio.token.hpp>
#include <pass.custody/pass.custody.db.hpp>
#include <otcbook/amax_math.hpp>
#include <otcbook/otcbook.hpp>
#include <otcconf/utils.hpp>
#include <otcsettle.hpp>
#include "aplink.farm/aplink.farm.hpp"

static constexpr eosio::name active_permission{"active"_n};

#define STAKE_CHANGED(account, quantity, memo) \
    {	metabalance::otcbook::stakechanged_action act{ _self, { {_self, active_permission} } };\
			act.send( account, quantity , memo );}


#define DEAL_NOTIFY(account, info, action_type, deal) \
    {	metabalance::otcbook::dealnotify_action act{ _self, { {_self, active_permission} } };\
			act.send( account, info , action_type, deal );}

#define REJECT_MERCHANT(account, reject_reason, curr) \
    {	metabalance::otcbook::reject_merchant_action act{ _self, { {_self, active_permission} } };\
			act.send( account, reject_reason , curr );}

#define ALLOT(bank, land_id, customer, quantity, memo) \
    {	aplink::farm::allot_action act{ bank, { {_self, active_perm} } };\
			act.send( land_id, customer, quantity , memo );}

using namespace metabalance;
using namespace std;
using namespace eosio;
using namespace wasm::safemath;
using namespace otc;

inline int64_t get_precision(const symbol &s) {
    int64_t digit = s.precision();
    CHECK(digit >= 0 && digit <= 18, "precision digit " + std::to_string(digit) + " should be in range[0,18]");
    return calc_precision(digit);
}

inline int64_t get_precision(const asset &a) {
    return get_precision(a.symbol);
}

asset otcbook::_calc_order_stakes(const asset &quantity) {
    // calc order quantity value by price
    auto stake_symbol = _conf().coin_as_stake.at(quantity.symbol);
    auto value = multiply_decimal64( quantity.amount, get_precision(stake_symbol), get_precision(quantity) );
    int64_t amount = divide_decimal64(value, order_stake_pct, percent_boost);
    return asset(amount, stake_symbol);
}

asset otcbook::_calc_deal_amount(const asset &quantity) {
    auto stake_symbol = _conf().coin_as_stake.at(quantity.symbol);
    auto value = multiply_decimal64( quantity.amount, get_precision(stake_symbol), get_precision(quantity) );
    return asset(value, stake_symbol);
}

asset otcbook::_calc_deal_fee(const asset &quantity) {
    // calc order quantity value by price
    auto stake_symbol = _conf().coin_as_stake.at(quantity.symbol);
    auto value = multiply_decimal64( quantity.amount, get_precision(stake_symbol), get_precision(quantity) );
    const auto & fee_pct = _conf().fee_pct;
    if (fee_pct  == 0) {
        return asset(0, stake_symbol);
    }
    int64_t amount = multiply_decimal64(value, fee_pct, percent_boost);
    amount = multiply_decimal64(amount, get_precision(stake_symbol), get_precision(quantity));
    return asset(amount, stake_symbol);
}

void otcbook::setconf(const name &conf_contract, const name& token_split_contract, const uint64_t& token_split_plan_id ) {
    require_auth( get_self() );    
    CHECK( is_account(conf_contract), "Invalid account of conf_contract");
    _gstate.conf_contract   = conf_contract;
    _gstate.token_split_contract = token_split_contract;
    _gstate.token_split_plan_id = token_split_plan_id;
    _check_split_plan( token_split_contract, token_split_plan_id, _self );
    _conf(true);
}

void otcbook::setadmin( const name& account, const bool& to_add) {
    require_auth( _self );

    auto admin = admin_t( account );
    auto found = _dbc.get( admin );
    CHECKC( found && !to_add || !found && to_add, err::PARAM_ERROR, "wrong params" )

    if (found)
        _dbc.del( admin );
    else 
        _dbc.set( admin, _self );
}

void otcbook::setmerchant( const name& sender, const merchant_info& mi ) {
    // CHECKC( has_auth(_conf().managers.at(otc::manager_type::admin)), err::NO_AUTH, "neither admin nor merchant" )
    _require_admin( sender );

    CHECKC(is_account(mi.account),err::ACCOUNT_INVALID,  "account invalid: " + mi.account.to_string());
    CHECKC(mi.merchant_name.size() < 32,err::NAME_TOO_LARGE, "merchant_name size too large: " + to_string(mi.merchant_name.size()) );
    CHECKC(mi.email.size() < 64,err::EMAIL_TOO_LARGE, "email size too large: " + to_string(mi.email.size()) );
    CHECKC(mi.merchant_detail.size() < 255,err::DETAL_TOO_LARGE, "mechant detail size too large: " + to_string(mi.merchant_detail.size()) );
    CHECKC(mi.memo.size() < max_memo_size,err::MEMO_TOO_LARGE, "memo size too large: " + to_string(mi.memo.size()) );
    CHECKC(mi.reject_reason.size() < 255,err::REJECT_REASON_TOO_LARGE, "reject reason size too large: " + to_string(mi.memo.size()) );

    auto merchant = merchant_t(mi.account);
    auto found = _dbc.get(merchant);
    CHECKC( found, err::ACCOUNT_NOT_FOUND, "merchant not existing: " + mi.account.to_string() )
    
    merchant.status = mi.status;
    merchant.updated_at = current_time_point();

    if ( mi.merchant_name.length() > 0 )   merchant.merchant_name      = mi.merchant_name;
    if ( mi.merchant_detail.length() > 0 ) merchant.merchant_detail    = mi.merchant_detail;
    if ( mi.email.length() > 0 )           merchant.email              = mi.email;
    if ( mi.memo.length() > 0 )            merchant.memo               = mi.memo;
    if ( mi.status == (uint8_t)merchant_status_t::REJECT && mi.reject_reason.length() > 1 )   {
        REJECT_MERCHANT(merchant.owner, mi.reject_reason, time_point_sec(current_time_point()) );
    }

    _dbc.set( merchant, get_self() );
}


void otcbook::remerchant( const merchant_info& mi) {
    require_auth(mi.account);

    CHECKC(mi.merchant_name.size() < 32,err::NAME_TOO_LARGE, "merchant_name size too large: " + to_string(mi.merchant_name.size()) );
    CHECKC(mi.email.size() < 64,err::EMAIL_TOO_LARGE, "email size too large: " + to_string(mi.email.size()) );
    CHECKC(mi.merchant_detail.size() < 255,err::DETAL_TOO_LARGE, "mechant detail size too large: " + to_string(mi.merchant_detail.size()) );
    CHECKC(mi.memo.size() < max_memo_size,err::MEMO_TOO_LARGE, "memo size too large: " + to_string(mi.memo.size()) );
    // CHECKC(mi.reject_reason.size() < 255,err::REJECT_REASON_TOO_LARGE, "reject reason size too large: " + to_string(mi.memo.size()) );

    auto merchant = merchant_t(mi.account);
    auto found = _dbc.get(merchant);
    CHECKC( found, err::ACCOUNT_NOT_FOUND, "merchant not found: " + mi.account.to_string() )
    CHECKC( merchant.status == (uint8_t)merchant_status_t::REJECT &&  mi.status == (uint8_t)merchant_status_t::REGISTERED, err::NO_AUTH, "merchant status is not reject.")
    merchant.status = (uint8_t)merchant_status_t::REGISTERED;
    merchant.updated_at = current_time_point();
    if ( mi.merchant_name.length() > 0 )   merchant.merchant_name      = mi.merchant_name;
    if ( mi.merchant_detail.length() > 0 ) merchant.merchant_detail    = mi.merchant_detail;
    if ( mi.email.length() > 0 )           merchant.email              = mi.email;
    if ( mi.memo.length() > 0 )            merchant.memo               = mi.memo;
    _dbc.set( merchant, get_self() );
}

void otcbook::delmerchant( const name& sender, const name& merchant_acct ) {
    _require_admin( sender );

    auto merchant = merchant_t( merchant_acct );
    CHECKC( _dbc.get(merchant), err::RECORD_NOT_FOUND, "merchant not found: " + merchant_acct.to_string() )

    _dbc.del( merchant );

}

/**
 * only merchant allowed to open orders
 */
void otcbook::openorder(const name& owner, const name& order_side, const set<name> &pay_methods, const asset& va_quantity, const asset& va_price,
    const asset& va_min_take_quantity,  const asset& va_max_take_quantity, const string &memo
){
    CHECKC(_conf().status == conf_status::RUNNING,err::UNINITIALIZED, "service is in maintenance");
    require_auth( owner );
    CHECKC( ORDER_SIDES.count(order_side) != 0,err::INVALID_ORDER_SIZE, "Invalid order side" );
    CHECKC( va_quantity.is_valid(),err::INVALID_QUANTITY, "Invalid quantity");
    CHECKC( va_price.is_valid(),err::INVALID_PRICE, "Invalid va_price");
    const auto& conf = _conf();
    CHECKC( va_price.symbol == conf.fiat_type,err::PAY_TYPE_NOT_ALLOW, "va price symbol not allow");
    CHECKC( conf.coin_as_stake.count(va_quantity.symbol),err::QUANTITY_SYMBOL_NOT_ALLOW, "va quantity symbol hasn't config stake asset");
    if (order_side == BUY_SIDE) {
        CHECKC( conf.buy_coins_conf.count(va_quantity.symbol) != 0,err::QUANTITY_SYMBOL_NOT_ALLOW, "va quantity symbol not allowed for buying" );
    } else {
        CHECKC( conf.sell_coins_conf.count(va_quantity.symbol) != 0,err::QUANTITY_SYMBOL_NOT_ALLOW, "va quantity symbol not allowed for selling" );
    }

    for (auto& method : pay_methods) {
        CHECKC( conf.pay_type.count(method) != 0,err::PAY_TYPE_NOT_ALLOW, "pay method illegal: " + method.to_string() );
    }

    CHECKC( va_quantity.amount > 0,err::QUANTITY_NOT_POSITIVE, "quantity must be positive");
    // TODO: min order quantity
    CHECKC( va_price.amount > 0,err::PRICE_NOT_POSITIVE, "va price must be positive" );
    // TODO: price range
    CHECKC( va_min_take_quantity.symbol == va_quantity.symbol,err::INVALID_MIN_QUANTITY_SYMBOL, "va_min_take_quantity Symbol mismatch with quantity" );
    CHECKC( va_max_take_quantity.symbol == va_quantity.symbol,err::INVALID_MAX_QUANTITY_SYMBOL, "va_max_take_quantity Symbol mismatch with quantity" );
    CHECKC( va_min_take_quantity.amount > 0 && va_min_take_quantity.amount <= va_quantity.amount,err::INVALID_MIN_QUANTITY,
        "invalid va_min_take_quantity amount" );
    CHECKC( va_max_take_quantity.amount > 0 && va_max_take_quantity.amount <= va_quantity.amount,err::INVALID_MAX_QUANTITY,
        "invalid va_max_take_quantity amount" );

    merchant_t merchant(owner);
    CHECKC( _dbc.get(merchant),err::ACCOUNT_NOT_FOUND, "merchant not found: " + owner.to_string() );
    CHECKC((merchant_status_t)merchant.status >= merchant_status_t::BASIC,err::ACCOUNT_STATE_MISMATCH,
        "merchant not enabled");

    auto stake_frozen = _calc_order_stakes(va_quantity); // TODO: process 70% used-rate of stake
    _frozen(merchant, stake_frozen);

    // TODO: check pos_staking_contract
    // if (_gstate.min_pos_stake_frozen.amount > 0) {
    // 	auto staking_con = _gstate.pos_staking_contract;
    // 	balances bal(staking_con, staking_con.value);
    // 	auto itr = bal.find(owner.value);
    // 	check( itr != bal.end(), "POS staking not found for: " + owner.to_string() );
    // 	check( itr->remaining >= _gstate.min_pos_stake_frozen, "POS Staking requirement not met" );
    // }

    order_t order;
    order.owner 				    = owner;
    order.va_price				    = va_price;
    order.va_quantity			    = va_quantity;
    order.stake_frozen              = stake_frozen;
    order.va_min_take_quantity      = va_min_take_quantity;
    order.va_max_take_quantity      = va_max_take_quantity;
    order.memo                      = memo;
    order.stake_frozen              = stake_frozen;
    order.status				    = (uint8_t)order_status_t::RUNNING;
    order.created_at			    = time_point_sec(current_time_point());
    order.va_frozen_quantity       = asset(0, va_quantity.symbol);
    order.va_fulfilled_quantity    = asset(0, va_quantity.symbol);
    order.accepted_payments         = pay_methods;
    order.merchant_name             = merchant.merchant_name;
    order.updated_at                = time_point_sec(current_time_point());


    if (order_side == BUY_SIDE) {
        buy_order_table_t orders(_self, _self.value);
        _gstate.buy_order_id++;
        order.id = _gstate.buy_order_id;
        orders.emplace( _self, [&]( auto& row ) {
            row = order;
        });
    } else {
        sell_order_table_t orders(_self, _self.value);
        _gstate.sell_order_id++;
        order.id = _gstate.sell_order_id;
        orders.emplace( _self, [&]( auto& row ) {
            row = order;
        });
    }
}

void otcbook::pauseorder(const name& owner, const name& order_side, const uint64_t& order_id) {
    require_auth( owner );

    merchant_t merchant(owner);
    CHECKC( _dbc.get(merchant),err::ACCOUNT_NOT_FOUND, "merchant not found: " + owner.to_string() );
    CHECKC( ORDER_SIDES.count(order_side) != 0,err::INVALID_ORDER_SIZE, "Invalid order side" );

    auto order_wrapper_ptr = (order_side == BUY_SIDE) ?
        buy_order_wrapper_t::get_from_db(_self, _self.value, order_id)
        : sell_order_wrapper_t::get_from_db(_self, _self.value, order_id);
    CHECKC( order_wrapper_ptr != nullptr,err::ORDER_NOT_FOUND, "order not found");
    const auto &order = order_wrapper_ptr->get_order();
    CHECKC( owner == order.owner,err::NO_AUTH, "have no access to close others' order");
    CHECKC( (order_status_t)order.status == order_status_t::RUNNING,err::ORDER_STATE_NOT_RUNNING, "order not running" );
    order_wrapper_ptr->modify(_self, [&]( auto& row ) {
        row.status = (uint8_t)order_status_t::PAUSED;
        row.updated_at = time_point_sec(current_time_point());
    });
}

void otcbook::resumeorder(const name& owner, const name& order_side, const uint64_t& order_id) {
    require_auth( owner );

    merchant_t merchant(owner);
    CHECKC( _dbc.get(merchant), err::ACCOUNT_NOT_FOUND, "merchant not found: " + owner.to_string() );
    CHECKC( ORDER_SIDES.count(order_side) != 0, err::INVALID_ORDER_SIZE, "Invalid order side" );

    auto order_wrapper_ptr = (order_side == BUY_SIDE) ?
        buy_order_wrapper_t::get_from_db(_self, _self.value, order_id)
        : sell_order_wrapper_t::get_from_db(_self, _self.value, order_id);
    CHECKC( order_wrapper_ptr != nullptr, err::ORDER_NOT_FOUND, "order not found");
    const auto &order = order_wrapper_ptr->get_order();
    CHECKC( owner == order.owner, err::NO_AUTH, "have no access to close others' order");
    CHECKC( (order_status_t)order.status == order_status_t::PAUSED, err::ORDER_STATE_NOT_RUNNING, "order not paused" );
    order_wrapper_ptr->modify(_self, [&]( auto& row ) {
        row.status = (uint8_t)order_status_t::RUNNING;
        row.updated_at = time_point_sec(current_time_point());
    });
}

void otcbook::closeorder(const name& owner, const name& order_side, const uint64_t& order_id) {
    require_auth( owner );

    merchant_t merchant(owner);
    CHECKC( _dbc.get(merchant), err::ACCOUNT_NOT_FOUND, "merchant not found: " + owner.to_string() );
    CHECKC( ORDER_SIDES.count(order_side) != 0, err::INVALID_ORDER_SIZE, "Invalid order side" );

    auto order_wrapper_ptr = (order_side == BUY_SIDE) ?
        buy_order_wrapper_t::get_from_db(_self, _self.value, order_id)
        : sell_order_wrapper_t::get_from_db(_self, _self.value, order_id);
    CHECKC( order_wrapper_ptr != nullptr, err::ORDER_NOT_FOUND, "order not found");
    const auto &order = order_wrapper_ptr->get_order();
    CHECKC( owner == order.owner, err::NO_AUTH, "have no access to close others' order");
    CHECKC( (uint8_t)order.status != (uint8_t)order_status_t::CLOSED, err::ORDER_STATE_NOT_CLOSED, "order already closed" );
    CHECKC( order.va_frozen_quantity.amount == 0, err::INVALID_QUANTITY, "order being processed" );
    CHECKC( order.va_quantity >= order.va_fulfilled_quantity, err::INVALID_QUANTITY, "order quantity insufficient" );

    _unfrozen(merchant, order.stake_frozen);

    _dbc.set( merchant , get_self());

    order_wrapper_ptr->modify(_self, [&]( auto& row ) {
        row.status = (uint8_t)order_status_t::CLOSED;
        row.closed_at = time_point_sec(current_time_point());
        row.updated_at  = time_point_sec(current_time_point());
    });
}

void otcbook::opendeal( const name& taker, const name& order_side, const uint64_t& order_id,
                        const asset& deal_quantity, const uint64_t& order_sn, const name& pay_type) {
    if(order_side == BUY_SIDE) {
        CHECKC(deal_quantity.symbol != USDTARC_SYMBOL, err::QUANTITY_SYMBOL_MISMATCH,  "deal quantity must not USDTARC_SYMBOL") 
    }
    _opendeal( taker, order_side, order_id, deal_quantity, order_sn,  pay_type);
} 

void otcbook::_opendeal( const name& taker, const name& order_side, const uint64_t& order_id,
                        const asset& deal_quantity, const uint64_t& order_sn, const name& pay_type) {
    require_auth( taker );

    auto conf = _conf();
    CHECKC( conf.status == conf_status::RUNNING, err::UNINITIALIZED, "service is in maintenance" );
    CHECKC( ORDER_SIDES.count(order_side) != 0, err::INVALID_ORDER_SIZE, "Invalid order side" );

    auto order_wrapper_ptr = (order_side == BUY_SIDE) ? buy_order_wrapper_t::get_from_db(_self, _self.value, order_id)
                                                      : sell_order_wrapper_t::get_from_db(_self, _self.value, order_id);
    CHECKC( order_wrapper_ptr != nullptr, err::ORDER_NOT_FOUND, "order not found");
    const auto &order = order_wrapper_ptr->get_order();
    CHECKC( order.owner != taker, err::NO_AUTH, "taker cannot be equal to maker" );
    CHECKC( deal_quantity.symbol == order.va_quantity.symbol, err::QUANTITY_SYMBOL_MISMATCH, "Token Symbol mismatch" );
    CHECKC( order.status == (uint8_t)order_status_t::RUNNING, err::ORDER_STATE_NOT_RUNNING, "order not running" );
    CHECKC( order.va_quantity >= order.va_frozen_quantity + order.va_fulfilled_quantity + deal_quantity,
                err::QUANTITY_MISMATCH, "Order's quantity insufficient" );
    CHECKC( deal_quantity >= order.va_min_take_quantity, err::INVALID_MIN_QUANTITY, "Order's min accept quantity not met!" );
    CHECKC( deal_quantity <= order.va_max_take_quantity, err::INVALID_MAX_QUANTITY, "Order's max accept quantity not met!" );

    auto now                    = current_time_point();

    blacklist_t::idx_t blacklist_tbl( _self, _self.value );
    auto blacklist_itr          = blacklist_tbl.find(taker.value);
    CHECKC( blacklist_itr == blacklist_tbl.end() || blacklist_itr->expired_at <= now,err::BLACKLISTED, "taker is blacklisted" )

    auto order_price            = order.va_price;
    auto order_maker            = order.owner;
    auto merchant_name          = order.merchant_name;

    deal_t::idx_t deals(_self, _self.value);
    auto ordersn_index 			= deals.get_index<"ordersn"_n>();

    CHECKC( ordersn_index.find(order_sn) == ordersn_index.end() ,err::ORDER_EXISTING, "order_sn already existing!" );
    auto deal_fee = _calc_deal_fee(deal_quantity);

    auto deal_id = deals.available_primary_key();
    _gstate.deal_id ++;
    // deals.emplace( taker, [&]( auto& row ) {
    deals.emplace( _self,       [&]( auto& row ) { //free user from paying ram fees
        row.id 					= _gstate.deal_id;
        row.order_side 			= order_side;
        row.merchant_name       = merchant_name;
        row.order_id 			= order_id;
        row.order_price			= order_price;
        row.deal_quantity		= deal_quantity;
        row.order_maker			= order_maker;
        row.order_taker			= taker;
        row.pay_type            = pay_type;
        row.status				= (uint8_t)deal_status_t::CREATED;
        row.arbit_status        = (uint8_t)arbit_status_t::UNARBITTED;
        row.created_at			= now;
        row.updated_at          = now;
        row.order_sn 			= order_sn;
        row.deal_fee            = deal_fee;
    });

    // // 添加交易到期表数据
    // deal_expiry_tbl deal_expiries(_self, _self.value);
    // deal_expiries.emplace( _self, [&]( auto& row ){
    //     row.deal_id = deal_id;
    //     row.expired_at 			= time_point_sec(created_at.sec_since_epoch() + _gstate.withhold_expire_sec);
    // });

    order_wrapper_ptr->modify(_self, [&]( auto& row ) {
        row.va_frozen_quantity 	+= deal_quantity;
        row.updated_at          = now;
    });

    deal_change_info deal_info;
    deal_info.deal_id       = _gstate.deal_id;
    deal_info.order_id      = order_id;
    deal_info.order_side    = order_side;
    deal_info.merchant      = order_maker;
    deal_info.taker         = taker;
    deal_info.status        = (uint8_t)deal_status_t::CREATED;
    deal_info.arbit_status  = (uint8_t)arbit_status_t::UNARBITTED;
    deal_info.quant         = deal_quantity;
    DEAL_NOTIFY(order_maker, conf.app_info, (uint8_t)deal_action_t::CREATE, deal_info);
}

/**
 * actively close the deal by order taker
 */
void otcbook::closedeal(const name& account, const uint8_t& account_type, const uint64_t& deal_id, const string& close_msg) {
    require_auth( account );

    _closedeal(account, account_type, deal_id, close_msg, false);
}

deal_t otcbook::_closedeal(const name& account, const uint8_t& account_type, const uint64_t& deal_id, const string& close_msg, const bool& by_transfer) {
    auto conf = _conf();
    deal_t::idx_t deals(_self, _self.value);
    auto deal_itr = deals.find(deal_id);
    CHECKC( deal_itr != deals.end(),err::ORDER_NOT_FOUND, "deal not found: " + to_string(deal_id) );
    auto status = (deal_status_t)deal_itr->status;
    CHECKC( (uint8_t)status != (uint8_t)deal_status_t::CLOSED,err::ORDER_STATE_CLOSED, "deal already closed: " + to_string(deal_id) );
    CHECKC( (uint8_t)status != (uint8_t)deal_status_t::CANCELLED,err::ORDER_STATE_CANCELLED, "deal already cancelled: " + to_string(deal_id) );
    auto arbit_status =  (arbit_status_t)deal_itr->arbit_status;
    auto merchant_paid_at = deal_itr->merchant_paid_at;

    switch ((account_type_t) account_type) {
    case account_type_t::USER:
        CHECKC( deal_itr->order_taker == account,err::NO_AUTH,  "taker account mismatched");
        break;
    case account_type_t::ADMIN:
        _require_admin( account );
        // CHECKC( _conf().managers.at(otc::manager_type::admin) == account,err::NO_AUTH,  "admin account mismatched");
        break;
    case account_type_t::ARBITER:
        CHECKC( deal_itr->arbiter == account,err::NO_AUTH,  "abiter account mismatched");
        break;
    case account_type_t::MERCHANT:
        CHECKC( deal_itr->order_maker == account, err::NO_AUTH, "merchant account mismatched");
        CHECKC( (uint8_t)status == (uint8_t)deal_status_t::MAKER_RECV_AND_SENT || 
            (uint8_t)status == (uint8_t)deal_status_t::TAKER_SENT,err::ORDER_STATE_MISMATCH, "can only close deal in status taker_sent or maker_recv");
        CHECKC( by_transfer || (merchant_paid_at + seconds(_conf().payed_timeout) < current_time_point()),err::TIME_NOT_EXPIRED, "deal is not expired.");
        break;
    default:
        CHECKC(false, err::ACCCOUNT_TYPE_MISMATCH, "account type not supported: " + to_string(account_type));
        break;
    }

    auto order_id = deal_itr->order_id;
    auto order_wrapper_ptr = (deal_itr->order_side == BUY_SIDE) ?
        buy_order_wrapper_t::get_from_db(_self, _self.value, order_id)
        : sell_order_wrapper_t::get_from_db(_self, _self.value, order_id);
    CHECKC( order_wrapper_ptr != nullptr, err::ORDER_NOT_FOUND, "order not found");
    const auto &order = order_wrapper_ptr->get_order();

    CHECKC( (uint8_t)order.status != (uint8_t)order_status_t::CLOSED,err::ORDER_STATE_CLOSED, "order already closed" );

    auto action = deal_action_t::CLOSE;
    const auto &order_maker  = deal_itr->order_maker;

    auto deal_quantity = deal_itr->deal_quantity;
    CHECKC( order.va_frozen_quantity >= deal_quantity,err::INVALID_QUANTITY, "Err: order frozen quantity smaller than deal quantity" );
    auto deal_fee= deal_itr->deal_fee;

    if ((account_type_t) account_type == account_type_t::MERCHANT || (account_type_t) account_type == account_type_t::USER) {
        CHECKC( deal_status_t::MAKER_RECV_AND_SENT == status || (deal_status_t::TAKER_SENT == status && by_transfer), 
            err::ORDER_STATE_MISMATCH,
            "can not process deal action:" + to_string((uint8_t)action)
                + " at status: " + to_string((uint8_t)status) );
    }

    auto now                        = current_time_point();
    auto stake_quantity             = _calc_order_stakes(deal_quantity);

    order_wrapper_ptr->modify(_self, [&]( auto& row ) {
        row.stake_frozen            -= stake_quantity;
        row.va_frozen_quantity      -= deal_quantity;
        row.va_fulfilled_quantity   += deal_quantity;
        row.updated_at              = now;
        if(row.stake_frozen.amount == 0 && row.va_frozen_quantity.amount == 0){
            row.status = (uint8_t)order_status_t::CLOSED;
            row.closed_at = now;
        }
    });

    deals.modify( *deal_itr, _self, [&]( auto& row ) {
        row.status                  = (uint8_t)deal_status_t::CLOSED;
        row.closed_at               = now;
        row.updated_at              = now;
        row.close_msg               = close_msg;
    });

    merchant_t merchant(order_maker);
    CHECKC( _dbc.get(merchant), err::ACCOUNT_NOT_FOUND,"merchant not found: " + order_maker.to_string() );
    _unfrozen(merchant, stake_quantity);

    if ( deal_fee.amount > 0) {
        _sub_balance(merchant, deal_fee, "fee:"+to_string(deal_id));
        TRANSFER( MBANK, _gstate.token_split_contract, deal_fee, std::string("plan:") + to_string( _gstate.token_split_plan_id) + ":" + to_string(deal_fee.amount) )
    }

    auto fee = deal_itr->deal_fee;
    auto deal_amount = _calc_deal_amount(deal_itr->deal_quantity);
    auto settle_arc = conf.managers.at(otc::manager_type::settlement);

    if (deal_amount.symbol == STAKE_USDT) {
        if (is_account(settle_arc)) {
            SETTLE_DEAL(settle_arc,
                        _self,
                        deal_id, 
                        deal_itr->order_maker,
                        deal_itr->order_taker, 
                        deal_amount,
                        fee,
                        0, 
                        deal_itr->created_at, 
                        deal_itr->closed_at);
        }
    }

    return *deal_itr;
}
void otcbook::canceldeal(const name& account, const uint8_t& account_type, const uint64_t& deal_id, bool is_taker_black) {
    require_auth( account );

    deal_t::idx_t deals(_self, _self.value);
    auto deal_itr = deals.find(deal_id);
    CHECKC( deal_itr != deals.end(),err::ORDER_NOT_FOUND, "deal not found: " + to_string(deal_id) );
    auto status = (deal_status_t)deal_itr->status;
    auto arbit_status =  (arbit_status_t)deal_itr->arbit_status;
    auto now = current_time_point();

    switch ((account_type_t) account_type) {
    case account_type_t::USER:
        switch ((deal_status_t) status) {
            case deal_status_t::CREATED:
                break;
            case deal_status_t::MAKER_ACCEPTED: {
                auto merchant_accepted_at = deal_itr->merchant_accepted_at;
                CHECKC(merchant_accepted_at + seconds(_conf().accepted_timeout) < now, err::TIME_NOT_EXPIRED, "deal is not expired.");
                break;
            }
            default:
                CHECKC( false, err::ORDER_STATE_MISMATCH,  "deal status need be CREATED or MAKER_ACCEPTED, deal_id:" + to_string(deal_id));
        }
        CHECKC( deal_itr->order_taker == account, err::NO_AUTH, "user account mismatched");
        break;
    case account_type_t::MERCHANT: {
        switch ((deal_status_t) status) {
            case deal_status_t::CREATED:
                break;
            case deal_status_t::MAKER_ACCEPTED: {
                auto merchant_accepted_at = deal_itr->merchant_accepted_at;
                CHECKC(merchant_accepted_at + seconds(_conf().accepted_timeout) < now, err::TIME_NOT_EXPIRED, "deal is not expired.");
                if (is_taker_black)
                    _set_blacklist(deal_itr->order_taker, default_blacklist_duration_second, get_self());
                break;
            }
            default:
                CHECKC( false,err::ORDER_STATE_MISMATCH,  "deal status need be CREATED or MAKER_ACCEPTED, deal_id:" + to_string(deal_id));
        }
        CHECKC( deal_itr->order_maker == account,err::NO_AUTH, "merchant account mismatched");
        break;
    }
    case account_type_t::ADMIN:
        _require_admin( account );
        // CHECKC( _conf().managers.at(otc::manager_type::admin) == account,err::NO_AUTH, "admin account mismatched");
        break;
    case account_type_t::ARBITER:
        CHECKC( deal_itr->arbiter == account,err::NO_AUTH, "abiter account mismatched");
        break;
    default:
        CHECKC(false,err::ACCCOUNT_TYPE_MISMATCH, "account type not supported: " + to_string(account_type));
        break;
    }

    auto order_id = deal_itr->order_id;
    auto order_wrapper_ptr = (deal_itr->order_side == BUY_SIDE) ?
        buy_order_wrapper_t::get_from_db(_self, _self.value, order_id)
        : sell_order_wrapper_t::get_from_db(_self, _self.value, order_id);
    CHECKC( order_wrapper_ptr != nullptr, err::ORDER_NOT_FOUND, "order not found");
    const auto &order = order_wrapper_ptr->get_order();

    CHECKC( (uint8_t)order.status != (uint8_t)order_status_t::CLOSED,err::ORDER_STATE_CLOSED, "order already closed" );

    auto order_status  = order.status;
    auto limit_seconds = seconds(deal_expired_second);
    if((account_type_t)account_type == account_type_t::USER) {
        if ((((time_point_sec(current_time_point())) - deal_itr->updated_at) > limit_seconds )  
            && (order.status == (uint8_t)order_status_t::RUNNING )){
            order_status = (uint8_t)order_status_t::PAUSED;
        }
    }

    deals.modify( *deal_itr, _self, [&]( auto& row ) {
            row.arbit_status = (uint8_t)arbit_status_t::UNARBITTED;
            row.status = (uint8_t)deal_status_t::CANCELLED;
            row.closed_at = time_point_sec(current_time_point());
            row.updated_at = time_point_sec(current_time_point());
            row.close_msg = "cancel deal";
        });

    auto deal_quantity = deal_itr->deal_quantity;


    // finished deal-canceled
    order_wrapper_ptr->modify(_self, [&]( auto& row ) {
        row.va_frozen_quantity -= deal_quantity;
        row.updated_at = time_point_sec(current_time_point());
        row.status = order_status;
    });
    
    if (deal_itr->deal_quantity.symbol == USDTARC_SYMBOL && deal_itr->order_side == BUY_SIDE) {
        auto deal_quantity = deal_itr->deal_quantity;
        deal_quantity.symbol = MUSDT_SYMBOL;
        _transfer_usdt(deal_itr->order_taker, deal_quantity, deal_itr->id);
    }
}

deal_t otcbook::_process(const name& account, const uint8_t& account_type, const uint64_t& deal_id, uint8_t action_type) {
    deal_t::idx_t deals(_self, _self.value);
    auto deal_itr = deals.find(deal_id);
    CHECKC( deal_itr != deals.end(), err::ORDER_NOT_FOUND, "deal not found: " + to_string(deal_id) );

    auto order_wrapper_ptr = ( deal_itr->order_side == BUY_SIDE ) ?
        buy_order_wrapper_t::get_from_db( _self, _self.value, deal_itr->order_id )
        : sell_order_wrapper_t::get_from_db( _self, _self.value, deal_itr->order_id) ;
    CHECKC( order_wrapper_ptr != nullptr,err::ORDER_NOT_FOUND, "order not found" );

    auto now = time_point_sec(current_time_point());
    switch ((account_type_t) account_type) {
    case account_type_t::MERCHANT:
        CHECKC( deal_itr->order_maker == account, err::NO_AUTH, "maker account mismatched" );
        break;
    case account_type_t::USER:
        CHECKC( deal_itr->order_taker == account, err::NO_AUTH, "taker account mismatched" );

        break;
    case account_type_t::ARBITER:
        CHECKC( deal_itr->arbiter == account, err::NO_AUTH, "arbiter account mismatched" );
        break;
    default:
        CHECKC( false, err::ACCCOUNT_TYPE_MISMATCH, "account type not supported: " + to_string(account_type) );
        break;
    }

    auto status = (deal_status_t)deal_itr->status;
    auto arbit_status = (arbit_status_t)deal_itr->arbit_status;
    deal_status_t limited_status = deal_status_t::NONE;
    account_type_t limited_account_type = account_type_t::NONE;
    arbit_status_t limit_arbit_status = arbit_status_t::UNARBITTED;
    deal_status_t next_status = deal_status_t::NONE;
    CHECKC( status != deal_status_t::CLOSED, err::ORDER_STATE_CLOSED,"deal already closed: " + to_string(deal_id) );
    CHECKC( status != deal_status_t::CANCELLED,err::ORDER_STATE_CANCELLED, "deal already cancelled: " + to_string(deal_id) );

#define DEAL_ACTION_CASE(_action, _limited_account_type, _limit_arbit_status,  _limited_status, _next_status) \
    case deal_action_t::_action:                                                        \
        limited_account_type = account_type_t::_limited_account_type;                   \
        limit_arbit_status = arbit_status_t::_limit_arbit_status;                        \
        limited_status = deal_status_t::_limited_status;                                \
        next_status = deal_status_t::_next_status;                                      \
        break;

    switch( (deal_action_t)action_type ){
        // /*               action              account_type  arbit_status, limited_status   next_status  */
        DEAL_ACTION_CASE(MAKER_ACCEPT,          MERCHANT,     UNARBITTED,   CREATED,         MAKER_ACCEPTED)
        DEAL_ACTION_CASE(TAKER_SEND,            USER,         UNARBITTED,   MAKER_ACCEPTED,  TAKER_SENT)
        DEAL_ACTION_CASE(MAKER_RECV_AND_SENT,   MERCHANT,     UNARBITTED,   TAKER_SENT,      MAKER_RECV_AND_SENT)
        default:
            check(false, "unsupported process deal action:" + to_string((uint8_t)action_type));
            break;
    }
    if (deal_itr->deal_quantity.symbol == USDTARC_SYMBOL && next_status == deal_status_t::MAKER_ACCEPTED && deal_itr->order_side == BUY_SIDE) {
        next_status = deal_status_t::TAKER_SENT;
        asset deal_quantity;
        deal_quantity.symbol = MUSDT_SYMBOL;
        deal_quantity.amount = deal_itr->deal_quantity.amount;
        _transfer_usdt(deal_itr->order_maker, deal_quantity, deal_itr->id);
    }

    if (limited_status != deal_status_t::NONE)
        CHECKC(limited_status == status,err::ORDER_STATE_MISMATCH, "can not process deal action:" + to_string((uint8_t)action_type)
             + " at status: " + to_string((uint8_t)status) );
    if (limited_account_type != account_type_t::NONE)
        CHECKC(limited_account_type == (account_type_t)account_type,err::ACCCOUNT_TYPE_MISMATCH,
            "can not process deal action:" + to_string((uint8_t)action_type)
             + " by account_type: " + to_string((uint8_t)account_type) );

    if ( (uint8_t)limit_arbit_status != (uint8_t)arbit_status_t::NONE)
        CHECKC(arbit_status == limit_arbit_status,err::ACCOUNT_STATE_MISMATCH,
            "can not process deal action:" + to_string((uint8_t)action_type)
             + " by arbit status: " + to_string((uint8_t)arbit_status) );

    deals.modify( *deal_itr, _self, [&]( auto& row ) {
        if (next_status != deal_status_t::NONE) {
            row.status = (uint8_t)next_status;
            row.updated_at = time_point_sec(current_time_point());
        }
        if((uint8_t)deal_action_t::MAKER_ACCEPT == action_type) {
            row.merchant_accepted_at = time_point_sec(current_time_point());
        }
        if((uint8_t)deal_action_t::MAKER_RECV_AND_SENT == action_type ) {
            row.merchant_paid_at = time_point_sec(current_time_point());
        }
    });


    if (account_type == (uint8_t)account_type_t::MERCHANT || account_type == (uint8_t)account_type_t::USER ) {
        deal_change_info deal_info;
        deal_info.deal_id       = deal_itr->id;
        deal_info.order_id      = deal_itr->order_id;
        deal_info.order_side    = deal_itr->order_side;
        deal_info.merchant      = deal_itr->order_maker;
        deal_info.taker         = deal_itr->order_taker;
        deal_info.status        = deal_itr->status;
        deal_info.arbit_status  = deal_itr->arbit_status;
        deal_info.quant         = deal_itr->deal_quantity;
        if ( account_type == (uint8_t)account_type_t::MERCHANT ) {
            DEAL_NOTIFY(deal_itr->order_taker, _conf().app_info, action_type, deal_info);
        } else {
            DEAL_NOTIFY(deal_itr->order_maker, _conf().app_info, action_type, deal_info);
        }
    }

    return *deal_itr;
}

void otcbook::processdeal(const name& account, const uint8_t& account_type, const uint64_t& deal_id, uint8_t action_type) {
    require_auth( account );
    _process(account, account_type, deal_id, action_type);
}


void otcbook::startarbit(const name& account, const uint8_t& account_type, const uint64_t& deal_id) {
    require_auth( account );

    deal_t::idx_t deals(_self, _self.value);
    auto deal_itr = deals.find(deal_id);
    CHECKC( deal_itr != deals.end(), err::ORDER_NOT_FOUND, "deal not found: " + to_string(deal_id) );

    auto order_wrapper_ptr = (deal_itr->order_side == BUY_SIDE) ?
        buy_order_wrapper_t::get_from_db(_self, _self.value, deal_itr->order_id)
        : sell_order_wrapper_t::get_from_db(_self, _self.value, deal_itr->order_id);
    CHECKC( order_wrapper_ptr != nullptr, err::ORDER_NOT_FOUND , "order not found");

    auto now = time_point_sec(current_time_point());

    switch ((account_type_t) account_type) {
    case account_type_t::MERCHANT:
        CHECKC( deal_itr->order_maker == account,err::NO_AUTH, "maker account mismatched");
        break;
    case account_type_t::USER:
        CHECKC( deal_itr->order_taker == account,err::NO_AUTH, "taker account mismatched");
        break;
    default:
        CHECKC(false,err::ACCCOUNT_TYPE_MISMATCH, "account type not supported: " + to_string(account_type));
        break;
    }

    auto arbiter = name(0);
    _rand_arbiter( deal_id, arbiter );
    auto status = (deal_status_t)deal_itr->status;
    auto arbit_status = (arbit_status_t)deal_itr->arbit_status;
    CHECKC( arbit_status == arbit_status_t::UNARBITTED , err::ORDER_STATE_UNARBITTED, "arbit already started: " + to_string(deal_id) );

    set<deal_status_t> can_arbit_status = {deal_status_t::MAKER_ACCEPTED, deal_status_t::TAKER_SENT, deal_status_t::MAKER_RECV_AND_SENT };
    CHECKC( can_arbit_status.count(status) != 0, err::ORDER_STATE_MISMATCH,"status illegal: " + to_string((uint8_t)status) );

    deals.modify( *deal_itr, _self, [&]( auto& row ) {
        row.arbit_status = (uint8_t)arbit_status_t::ARBITING;
        row.arbiter = arbiter;
        row.updated_at = time_point_sec(current_time_point());
       });
}

void otcbook::closearbit(const name& account, const uint64_t& deal_id, const uint8_t& arbit_result) {
    require_auth( account );

    deal_t::idx_t deals(_self, _self.value);
    auto deal_itr = deals.find(deal_id);
    CHECKC( deal_itr != deals.end(), err::ORDER_NOT_FOUND, "deal not found: " + to_string(deal_id) );

    auto order_wrapper_ptr = (deal_itr->order_side == BUY_SIDE) ?
        buy_order_wrapper_t::get_from_db(_self, _self.value, deal_itr->order_id)
        : sell_order_wrapper_t::get_from_db(_self, _self.value, deal_itr->order_id);
    CHECKC( order_wrapper_ptr != nullptr, err::ORDER_NOT_FOUND, "order not found");

    auto now = time_point_sec(current_time_point());
    CHECKC( deal_itr->arbiter == account, err::NO_AUTH , "arbiter account mismatched");

    auto status = (deal_status_t)deal_itr->status;
    auto arbit_status = (arbit_status_t)deal_itr->arbit_status;
    const auto &order_taker  = deal_itr->order_taker;
    const auto &order_maker  = deal_itr->order_maker;
    CHECKC( arbit_status == arbit_status_t::ARBITING, err::ORDER_STATE_NOT_ARBITING, "arbit isn't arbiting: " + to_string(deal_id) );

    auto deal_status = (uint8_t)deal_status_t::CLOSED;
    if (arbit_result == 0) {
        deal_status =  (uint8_t)deal_status_t::CANCELLED;
    }

    deals.modify( *deal_itr, _self, [&]( auto& row ) {
            row.arbit_status = uint8_t(arbit_result == 0 ? arbit_status_t::CLOSENOFINE : arbit_status_t::CLOSEWITHFINE );
            row.status = (uint8_t)deal_status_t::CLOSED;
            row.closed_at = time_point_sec(current_time_point());
            row.updated_at = time_point_sec(current_time_point());
        });

    auto deal_quantity = deal_itr->deal_quantity;
    auto deal_fee = deal_itr->deal_fee;
    auto deal_price = deal_itr->order_price;
    auto deal_amount = _calc_deal_amount(deal_quantity);
    auto order_id = deal_itr->order_id;

    if (arbit_result == 0) {
        // finished deal-canceled
        order_wrapper_ptr->modify(_self, [&]( auto& row ) {
            row.va_frozen_quantity -= deal_quantity;
            row.updated_at = time_point_sec(current_time_point());
        });

        _update_arbiter_info(account, deal_quantity, false);

    } else {
        // end deal - finished
        auto stake_quantity = _calc_order_stakes(deal_quantity);
        order_wrapper_ptr->modify(_self, [&]( auto& row ) {
            row.stake_frozen -= stake_quantity;
            row.va_frozen_quantity -= deal_quantity;
            row.va_fulfilled_quantity += deal_quantity;
            row.updated_at = time_point_sec(current_time_point());
        });

        //sub arbit fine
        merchant_t merchant(order_maker);
        CHECKC( _dbc.get(merchant),err::ACCOUNT_NOT_FOUND, "merchant not found: " + order_maker.to_string() );
        _unfrozen(merchant, stake_quantity);
        _sub_balance(merchant, stake_quantity, "arbit fine:"+to_string(deal_id));

        //refund

        TRANSFER(_conf().stake_assets_contract.at(stake_quantity.symbol), order_taker, 
        stake_quantity, "arbit fine: "+to_string(deal_id));
        _update_arbiter_info(account, deal_quantity, true);
   }
}

void otcbook::cancelarbit( const uint8_t& account_type, const name& account, const uint64_t& deal_id )
{
    require_auth( account );

    deal_t::idx_t deals(_self, _self.value);
    auto deal_itr = deals.find(deal_id);
    CHECKC( deal_itr != deals.end(),err::ORDER_NOT_FOUND, "deal not found: " + to_string(deal_id) );
    auto arbit_status = (arbit_status_t)deal_itr->arbit_status;

    CHECKC( arbit_status == arbit_status_t::ARBITING,err::ORDER_STATE_NOT_ARBITING, "deal is not arbiting" );
    auto status = deal_itr->status;

    switch ((account_type_t) account_type) {
    case account_type_t::MERCHANT:
        CHECKC( deal_itr->order_maker == account, err::NO_AUTH, "maker account mismatched");
        CHECKC( status == (uint8_t)deal_status_t::MAKER_ACCEPTED, err::ORDER_STATE_MAKER_ACCEPTED,
                    "arbiting only can be cancelled at TAKER_SENT or MAKER_ACCEPTED");
        break;
    // case account_type_t::USER:
    //     check( deal_itr->order_taker == account, "taker account mismatched");
    //     check( status == (uint8_t)deal_status_t::MAKER_RECV_AND_SENT, "arbiting only can be cancelled at MAKER_RECV_AND_SENT");
    //     break;
    default:
        CHECKC(false, err::ACCCOUNT_TYPE_MISMATCH,  "account type not supported: " + to_string(account_type));
        break;
    }

    auto now = time_point_sec(current_time_point());
    deals.modify( *deal_itr, _self, [&]( auto& row ) {
        row.arbit_status = (uint8_t)arbit_status_t::UNARBITTED;
        row.updated_at = now;
    });

}

void otcbook::resetdeal(const name& account, const uint64_t& deal_id){

    _require_admin( account );

    // CHECK( _conf().managers.at(otc::manager_type::admin) == account, "Only admin allowed" );

    deal_t::idx_t deals(_self, _self.value);
    auto deal_itr = deals.find(deal_id);
    CHECKC( deal_itr != deals.end(), err::ORDER_NOT_FOUND,"deal not found: " + to_string(deal_id) );

    auto status = (deal_status_t)deal_itr->status;
    CHECKC( status != deal_status_t::CLOSED,err::ORDER_STATE_CLOSED, "deal already closed: " + to_string(deal_id) );
    CHECKC( status != deal_status_t::CREATED, err::ORDER_STATE_CREATED, "deal no need to reverse" );

    auto now = time_point_sec(current_time_point());
    deals.modify( *deal_itr, _self, [&]( auto& row ) {
        row.status = (uint8_t)deal_status_t::CREATED;
        row.updated_at = time_point_sec(current_time_point());
    });
}

void otcbook::withdraw(const name& owner, asset quantity){
    auto conf = _conf();
    CHECKC( conf.status == conf_status::RUNNING,err::UNINITIALIZED, "service is in maintenance" );
    CHECKC( has_auth(owner) || has_auth(_self),err::NO_AUTH, "neither owner nor self" );
    CHECKC( quantity.amount > 0, err::QUANTITY_NOT_POSITIVE, "quanity must be positive" );
    CHECKC( quantity.symbol.is_valid(), err::QUANTITY_SYMBOL_MISMATCH, "Invalid quantity symbol name" );
    CHECKC( _conf().stake_assets_contract.count(quantity.symbol), err::QUANTITY_SYMBOL_MISMATCH, "Token Symbol not allowed" );

    merchant_t merchant(owner);
    CHECKC( _dbc.get(merchant),err::ACCOUNT_NOT_FOUND, "merchant not found: " + owner.to_string() );
    auto state = (merchant_status_t)merchant.status;
    CHECKC(state >= merchant_status_t::BASIC || state == merchant_status_t::DISABLED,err::ACCOUNT_STATE_MISMATCH, "merchant not enabled");

    auto limit_seconds = seconds(general_withdraw_limit_second);
    switch( state ) {
    case merchant_status_t::GOLD:
        limit_seconds = seconds(golden_withdraw_limit_second);
        break;
    case merchant_status_t::DIAMOND:
        limit_seconds = seconds(diamond_withdraw_limit_second);
        break;
    case merchant_status_t::BLUESHILED:
        limit_seconds = seconds(blueshiled_withdraw_limit_second);
        break;
    default:
        break;
    }
    CHECKC( (time_point_sec(current_time_point())-merchant.updated_at) > limit_seconds,err::TIME_NOT_REACHED,
        "Can only withdraw after " + to_string(int(limit_seconds.to_seconds()/seconds_per_day)) + " days from fund changed");

    _sub_balance(merchant, quantity, "merchant withdraw");

    TRANSFER( _conf().stake_assets_contract.at(quantity.symbol), owner, quantity, "merchant withdraw" )
}

void otcbook::ontransfer(name from, name to, asset quantity, string memo){
    if(_self == from || to != _self) return;
    CHECKC( _conf().stake_assets_contract.count(quantity.symbol),err::QUANTITY_SYMBOL_MISMATCH, "Token Symbol not allowed" );
    CHECKC( _conf().stake_assets_contract.at(quantity.symbol) == get_first_receiver(),err::QUANTITY_SYMBOL_NOT_ALLOW, "Token Symbol not allowed" );
  
    if(memo.empty()){
        _deposit(from, to, quantity, memo);
    }
    else {
        vector<string_view> memo_params = split(memo, ":"); 
        if (memo_params[0] == "apply" && memo_params.size() == 4) {
            _merchant_apply(from, quantity, memo_params);
        }
        else if (memo_params[0] == "opendeal" && memo_params.size() == 4) {
            quantity.symbol = USDTARC_SYMBOL;
            _transfer_open_deal(from, quantity, memo_params);
        }
        else if (memo_params[0] == "process" && memo_params.size() == 4) {
            _transfer_process_deal(from, quantity, memo_params);
        }
        else if (memo_params[0] == "close" && memo_params.size() == 3) {
            _transfer_close_deal(from, quantity, memo_params);
        } else {
            _deposit(from, to, quantity, memo);
        }
    }
}
/*************** Begin of eosio.token transfer trigger function ******************/
/**
 * This happens when a merchant decides to open sell orders
 */
void otcbook::_deposit(name from, name to, asset quantity, string memo) {
    if(_self == from || to != _self) return;

    CHECKC( _conf().stake_assets_contract.count(quantity.symbol),err::QUANTITY_SYMBOL_MISMATCH, "Token Symbol not allowed: " + quantity.to_string() );
    CHECKC( _conf().stake_assets_contract.at(quantity.symbol) == get_first_receiver(),err::QUANTITY_SYMBOL_NOT_ALLOW, "Token Contract not allowed: " 
                                                + _conf().stake_assets_contract.at(quantity.symbol).to_string() );
    merchant_t merchant(from);
    CHECKC(_dbc.get( merchant ),err::ACCOUNT_NOT_FOUND,"merchant is not set, from:" + from.to_string()+ ",to:" + to.to_string());
    CHECKC((merchant_status_t)merchant.status >= merchant_status_t::BASIC,err::ACCOUNT_STATE_MISMATCH,
        "merchant not enabled");
    _add_balance(merchant, quantity, "merchant deposit");
}


void otcbook::setblacklist(const name& from, const name& account, uint64_t duration_second) {
    // require_auth( _conf().managers.at(otc::manager_type::admin) );
    _require_admin(from);
    CHECKC( is_account(account),err::ACCOUNT_INVALID, "account does not exist: " + account.to_string() );
    CHECKC( duration_second <= max_blacklist_duration_second,err::TIME_TOO_LARGE,
           "duration_second too large than: " + to_string(max_blacklist_duration_second));

   _set_blacklist(account, duration_second, from);
}

const fiat_conf_t& otcbook::_conf(bool refresh/* = false*/) {
    
    CHECKC(_gstate.conf_contract.value != 0,err::SYSTEM_ERROR, "Invalid conf_table");
    
    fiat_conf_t::idx_t conf_tbl(_gstate.conf_contract,_gstate.conf_contract.value);
    auto itr = conf_tbl.find( _self.value );
    CHECKC( itr != conf_tbl.end(),err::CONF_NOT_FOUND, "conf table not existed in contract: " + _gstate.conf_contract.to_string());
    return conf_tbl.get( _self.value );
}

void otcbook::stakechanged(const name& account, const asset &quantity, const string& memo){
    require_auth(get_self());
    require_recipient(account);
}

void otcbook::dealnotifyv2(const name& account, const AppInfo_t &info, const uint8_t action_type, const deal_change_info& deal){
    require_auth(get_self());
    require_recipient(account);
}

void otcbook::rejectmerch(const name& account, const string& reject_reason, const time_point_sec& curr_ts){
    require_auth(get_self());
    require_recipient(account);
}

void otcbook::setdearbiter(const uint64_t& deal_id, const name& new_arbiter) {
    require_auth( _self );

    deal_t::idx_t deals(_self, _self.value);
    auto deal_itr = deals.find(deal_id);
    CHECKC( deal_itr != deals.end(),err::ORDER_NOT_FOUND , "deal not found: " + to_string(deal_id) );

    deals.modify(*deal_itr, _self, [&]( auto& row ) {
        row.arbiter = new_arbiter;
    });
}

void otcbook::_set_blacklist(const name& account, uint64_t duration_second, const name& payer) {
    blacklist_t::idx_t blacklist_tbl(_self, _self.value);
    auto blacklist_itr = blacklist_tbl.find(account.value);
    if (duration_second > 0) {
        blacklist_tbl.set( account.value, payer, [&]( auto& row ) {
            row.account     = account;
            row.expired_at  = current_time_point() + eosio::seconds(duration_second);
        });
    } else {
        blacklist_tbl.erase_by_pk(account.value);
    }
}

void otcbook::_add_balance(merchant_t& merchant, const asset& quantity, const string & memo){
    merchant.assets[quantity.symbol].balance += quantity.amount;
    merchant.updated_at = current_time_point();
    _dbc.set( merchant , get_self());
    if(memo.length() > 0) STAKE_CHANGED(merchant.owner, quantity, memo);
}

void otcbook::_sub_balance(merchant_t& merchant, const asset& quantity, const string & memo){
    CHECKC( merchant.assets[quantity.symbol].balance >= quantity.amount,err::QUANTITY_INSUFFICIENT, "merchant stake balance quantity insufficient");
    merchant.assets[quantity.symbol].balance -= quantity.amount;
    merchant.updated_at = current_time_point();
    _dbc.set( merchant , get_self());
    if(memo.length() > 0) STAKE_CHANGED(merchant.owner, -quantity, memo);
}

void otcbook::_frozen(merchant_t& merchant, const asset& quantity){
    CHECKC( merchant.assets[quantity.symbol].balance >= quantity.amount,err::QUANTITY_INSUFFICIENT, "merchant stake balance quantity insufficient");
    merchant.assets[quantity.symbol].balance -= quantity.amount;
    merchant.assets[quantity.symbol].frozen += quantity.amount;
    merchant.updated_at = current_time_point();
    _dbc.set( merchant , get_self());
}


void otcbook::_unfrozen(merchant_t& merchant, const asset& quantity){
    CHECKC( merchant.assets[quantity.symbol].frozen >= quantity.amount,err::QUANTITY_FROZEN_INSUFFICIENT, "merchant stake frozen quantity insufficient");
    merchant.assets[quantity.symbol].frozen -= quantity.amount;
    merchant.assets[quantity.symbol].balance += quantity.amount;
    merchant.updated_at = current_time_point();
    _dbc.set( merchant , get_self());
}


void otcbook::_merchant_apply(name from, asset quantity, vector<string_view> memo_params) {

    string merchant_name = string(memo_params[1]);
    string merchant_detail = string(memo_params[2]);
    string email = string(memo_params[3]);

    CHECKC(merchant_name.size() < 20, err::NAME_TOO_LARGE, "merchant_name size too large: " + to_string(merchant_name.size()) );
    CHECKC(merchant_detail.size() < 255,err::DETAL_TOO_LARGE, "merchant_detail size too large: " + to_string(merchant_detail.size()) );
    CHECKC(email.size() < 64,err::EMAIL_TOO_LARGE, "email size too large: " + to_string(email.size()) );

    merchant_t merchant(from);
    CHECKC(!_dbc.get( merchant ),err::ACCOUNT_EXISING,"merchant is existed");
    
    merchant.merchant_name = merchant_name;
    merchant.merchant_detail = merchant_detail;
    merchant.email = email;
    merchant.status = (uint8_t)merchant_status_t::BASIC;
    _add_balance(merchant, quantity, "merchant deposit");
    _dbc.set(merchant, get_self());
}

void otcbook::_transfer_open_deal(name from, asset quantity, vector<string_view> memo_params) {

    // CHECKC( quantity.symbol == MUSDT_SYMBOL,  err::SYMBOL_MISMATCH, "quantity symbol must musdt");
    uint64_t order_id = to_uint64(memo_params[1], "order id param error");
    uint64_t order_sn = to_uint64(memo_params[2], "order sn param error");
    name pay_type = name(memo_params[3]);
    _opendeal( from, BUY_SIDE, order_id, quantity,  order_sn, pay_type);
}

void otcbook::_transfer_process_deal(name from, asset quantity, vector<string_view> memo_params) {
    uint8_t account_type = to_uint8(memo_params[1], "account_type id param error");
    uint64_t deal_id = to_uint64(memo_params[2], "deal id param error");
    uint8_t action_type = to_uint64(memo_params[3], "action_type id param error");
    deal_t deal = _process(from, account_type, deal_id, action_type);
    auto stake_coin_type = _conf().coin_as_stake.at(deal.deal_quantity.symbol);
    auto stake_amount = multiply_decimal64( deal.deal_quantity.amount, get_precision(stake_coin_type), get_precision(deal.deal_quantity.symbol));
    CHECKC( asset(stake_amount, stake_coin_type) == quantity, err::QUANTITY_MISMATCH, "quantity must eqault to deal quantity" )
    TRANSFER( get_first_receiver(), from == deal.order_maker? deal.order_taker : deal.order_maker, 
        quantity, "metabalance deal: " + to_string(deal.id) );
}

void otcbook::_transfer_close_deal(name from, asset quantity, vector<string_view> memo_params) {
    uint8_t account_type = to_uint8(memo_params[1], "account_type id param error");
    uint64_t deal_id = to_uint64(memo_params[2], "deal id param error");
    deal_t deal = _closedeal(from, account_type, deal_id, "auto close by transfer", true);
    auto stake_coin_type = _conf().coin_as_stake.at(deal.deal_quantity.symbol);
    auto stake_amount = multiply_decimal64( deal.deal_quantity.amount, get_precision(stake_coin_type), get_precision(deal.deal_quantity.symbol));
    CHECKC( asset(stake_amount, stake_coin_type) == quantity, err::QUANTITY_MISMATCH, "quantity must eqault to deal quantity" )
    TRANSFER( get_first_receiver(), from == deal.order_maker? deal.order_taker : deal.order_maker, 
        quantity, "metabalance deal: " + to_string(deal.id) );
}

void otcbook::_transfer_usdt(name to, asset quantity, uint64_t deal_id) {
    TRANSFER( MT_BANK, to, quantity, "metabalance deal: " + to_string(deal_id) );
}


void otcbook::addarbiter(const name& sender, const name& account, const string& email) {
    _require_admin( sender );

    CHECKC(is_account(account), err::ACCOUNT_INVALID,  "account not existed: " +  account.to_string());

    auto arbiter = arbiter_t(account);
    if ( !_dbc.get(arbiter) ){   
        _gstate.arbiter_count = _gstate.arbiter_count + 1;
    }
     arbiter.email = email;
    // CHECKC( !_dbc.get(arbiter), err::ACCOUNT_EXISING, "arbiter already exists: " + account.to_string() );
    _dbc.set( arbiter, get_self());
}

void otcbook::delarbiter(const name& sender, const name& account) {
    _require_admin( sender );

    auto arbiter = arbiter_t(account);
    CHECKC( _dbc.get(arbiter), err::ACCOUNT_NOT_FOUND, "arbiter not found: " + account.to_string() );

    _dbc.del( arbiter);
    _gstate.arbiter_count = _gstate.arbiter_count - 1;
}

void otcbook::setarbitcnt ( const uint64_t count) {

    require_auth( _self );
    _gstate.arbiter_count = count;
}

void otcbook::_rand_arbiter( const uint64_t deal_id, name& arbiter ) {

    uint64_t rand = deal_id % _gstate.arbiter_count;

    arbiter_t::idx_t arbiter_idx( get_self(), get_self().value);
    auto itr = arbiter_idx.begin();
    if (rand > 0) {
        uint64_t curr_num = 0;
        for( ; itr != arbiter_idx.end(); itr++){
            curr_num ++;
            if ( rand <= curr_num)
                break;
        }
    }
    
    arbiter = itr->account;
}

void otcbook::_check_split_plan( const name& token_split_contract, const uint64_t& token_split_plan_id, const name& scope ) {
    custody::split_plan_t::idx_t split_t( token_split_contract, scope.value );
    auto split_itr = split_t.find( token_split_plan_id );
    CHECKC( split_itr != split_t.end(), err::SPLIT_NOT_FOUND,"token split plan not found, id:" + to_string(token_split_plan_id));
}

void otcbook::_update_arbiter_info( const name& account, const asset& quant, const bool& closed) {
    auto arbiter = arbiter_t(account);
    CHECKC( _dbc.get(arbiter), err::ACCOUNT_NOT_FOUND, "arbiter not found: " + account.to_string() );
    if ( closed ) {
        arbiter.closed_case_num  = arbiter.closed_case_num + 1;
    } else {
        arbiter.failed_case_num  = arbiter.failed_case_num + 1;
    }

    arbiter.total_quant.amount += quant.amount;
    _dbc.set( arbiter, get_self());
}

void otcbook::_require_admin(const name& account) {
    require_auth( account );
    // CHECKC( _conf().managers.at(otc::manager_type::admin) == account,err::NO_AUTH, "Only admin allowed" );

    auto admin = admin_t( account );
    CHECKC( _dbc.get( admin ), err::ACCOUNT_NOT_FOUND, "not admin: " + account.to_string() )
}