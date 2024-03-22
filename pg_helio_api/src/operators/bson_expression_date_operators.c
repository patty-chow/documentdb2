/*-------------------------------------------------------------------------
 * Copyright (c) Microsoft Corporation.  All rights reserved.
 *
 * src/bson/bson_expression_date_operators.c
 *
 * Object Operator expression implementations of BSON.
 *
 *-------------------------------------------------------------------------
 */

#include <postgres.h>
#include <pgtime.h>
#include <fmgr.h>
#include <math.h>
#include <utils/datetime.h>
#include <utils/timestamp.h>
#include <utils/builtins.h>

#include "io/helio_bson_core.h"
#include "operators/bson_expression.h"
#include "operators/bson_expression_operators.h"
#include "utils/mongo_errors.h"
#include "metadata/metadata_cache.h"

#define MILLISECONDS_IN_SECOND 1000L
#define SECONDS_IN_MINUTE 60
#define MINUTES_IN_HOUR 60
#define DAYS_IN_WEEK 7

/*
 * This represents unix ms for 0001-01-01.
 * Significance for this is it's used in dateFromParts.
 * This is the base underlying year for calculations we add year, month, day, hour, minute, seconds interval to this timestamp.
 */
#define DATE_FROM_PART_START_DATE_MS -62135596800000L

/* --------------------------------------------------------- */
/* Type declaration */
/* --------------------------------------------------------- */

/* Enum that defines the posible parts of a date/timestamp in Mongo. */
typedef enum DatePart
{
	DatePart_Hour = 0,
	DatePart_Minute = 1,
	DatePart_Second = 2,
	DatePart_Millisecond = 3,
	DatePart_Year = 4,
	DatePart_Month = 5,
	DatePart_Week = 6,
	DatePart_DayOfYear = 7,
	DatePart_DayOfMonth = 8,
	DatePart_DayOfWeek = 9,
	DatePart_IsoWeekYear = 10,
	DatePart_IsoWeek = 11,
	DatePart_IsoDayOfWeek = 12,
} DatePart;

static const char *isoDateFormat = "IYYY-IW-ID";
static const char *defaultTimezone = "UTC";

static const char *monthNamesCamelCase[12] = {
	"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};
static const char *monthNamesUpperCase[12] = {
	"JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"
};
static const char *monthNamesLowerCase[12] = {
	"jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec"
};

/* Struct that represents the parsed arguments to a $dateFromParts expression. */
typedef struct DollarDateFromParts
{
	AggregationExpressionData year;
	AggregationExpressionData isoWeekYear;
	AggregationExpressionData month;
	AggregationExpressionData isoWeek;
	AggregationExpressionData isoDayOfWeek;
	AggregationExpressionData day;
	AggregationExpressionData hour;
	AggregationExpressionData minute;
	AggregationExpressionData second;
	AggregationExpressionData millisecond;
	AggregationExpressionData timezone;
	bool isISOWeekDate;
} DollarDateFromParts;

/* Struct that represents the bson_value_t arguments to a $dateFromParts input values. */
typedef struct DollarDateFromPartsBsonValue
{
	bson_value_t year;
	bson_value_t isoWeekYear;
	bson_value_t month;
	bson_value_t isoWeek;
	bson_value_t isoDayOfWeek;
	bson_value_t day;
	bson_value_t hour;
	bson_value_t minute;
	bson_value_t second;
	bson_value_t millisecond;
	bson_value_t timezone;
} DollarDateFromPartsBsonValue;


/* --------------------------------------------------------- */
/* Forward declaration */
/* --------------------------------------------------------- */
static inline bool IsDollarDatePartsInputConstant(DollarDateFromParts *datePart);
static inline bool IsInputForDatePartNull(
	DollarDateFromPartsBsonValue *dateFromPartsValue, bool isIsoWeekDate);
static void HandleDatePartOperator(pgbson *doc, const bson_value_t *operatorValue,
								   ExpressionResult *expressionResult,
								   const char *operatorName, DatePart datePart);
static bool ParseDatePartOperatorArgument(const bson_value_t *operatorValue,
										  const char *operatorName,
										  bson_value_t *dateExpression,
										  bson_value_t *timezoneExpression);
static void ParseInputForDateFromParts(const bson_value_t *argument, bson_value_t *year,
									   bson_value_t *isoWeekYear, bson_value_t *month,
									   bson_value_t *isoWeek,
									   bson_value_t *isoDayOfWeek, bson_value_t *day,
									   bson_value_t *hour, bson_value_t *minute,
									   bson_value_t *second, bson_value_t *millisecond,
									   bson_value_t *timezone, bool *isIsoWeekDate);
static ExtensionTimezone ParseTimezone(StringView timezone);
static int64_t ParseUtcOffset(StringView offset);
static int32_t DetermineUtcOffsetForEpochWithTimezone(int64_t unixEpoch,
													  ExtensionTimezone timezone);
static bool TryParseTwoDigitNumber(StringView str, uint32_t *result);
static uint32_t GetDatePartFromPgTimestamp(Datum pgTimestamp, DatePart datePart);
static inline int GetDayOfWeek(int year, int month, int day);
static Datum GetIntervalFromDatePart(int64 year, int64 month, int64 week, int64 day, int64
									 hour, int64 minute, float8 second);
static int GetIsoWeeksForYear(int64 year);
static Datum GetPgTimestampAdjustedToTimezone(Datum timestamp, ExtensionTimezone
											  timezoneToApply);
static Datum GetPgTimestampFromEpochWithTimezone(int64_t epochInMs,
												 ExtensionTimezone timezone);
static Datum GetPgTimestampFromUnixEpoch(int64_t epochInMs);
static StringView GetDateStringWithFormat(int64_t dateInMs, ExtensionTimezone timezone,
										  StringView format);
static inline void SetDefaultValueForDatePart(bson_value_t *datePart, bson_type_t
											  bsonType, int64 defaultValue);
static void SetResultForDateFromParts(DollarDateFromPartsBsonValue *dateFromPartsValue,
									  ExtensionTimezone timezoneToApply,
									  bson_value_t *result);
static void SetResultForDateFromIsoParts(DollarDateFromPartsBsonValue *dateFromPartsValue,
										 ExtensionTimezone
										 timezoneToApply,
										 bson_value_t *result);
static void ValidateDatePart(DatePart datePart, bson_value_t *inputValue, char *inputKey);
static void ValidateInputForDateFromParts(
	DollarDateFromPartsBsonValue *dateFromPartsValue, bool isIsoWeekDate);

/* These 3 methods are specialized for creating a date string. */
static void WriteCharAndAdvanceBuffer(char **buffer, const char *end, char value);
static int WriteInt32AndAdvanceBuffer(char **buffer, const char *end, int32_t value);
static void WritePaddedUInt32AndAdvanceBuffer(char **buffer, const char *end, int padding,
											  uint32_t value);


/* Helper method that throws the error for an invalid timezone argument. */
static inline void
pg_attribute_noreturn()
ThrowInvalidTimezoneIdentifier(const char * identifier)
{
	ereport(ERROR, (errcode(MongoLocation40485), errmsg(
						"unrecognized time zone identifier: \"%s\"", identifier)));
}

/* Helper method that throws common Location40517 when a string is not provided for the timezone argument. */
static inline void
pg_attribute_noreturn()
ThrowLocation40517Error(bson_type_t foundType)
{
	ereport(ERROR, (errcode(MongoLocation40517), errmsg(
						"timezone must evaluate to a string, found %s",
						BsonTypeName(foundType)),
					errhint("timezone must evaluate to a string, found %s",
							BsonTypeName(foundType))));
}

/* Helper that validates a date value is in the valid range 0-9999. */
static inline void
ValidateDateValueIsInRange(uint32_t value)
{
	if (value > 9999)
	{
		ereport(ERROR, (errcode(MongoLocation18537), errmsg(
							"Could not convert date to string: date component was outside the supported range of 0-9999: %d",
							value),
						errhint(
							"Could not convert date to string: date component was outside the supported range of 0-9999: %d",
							value)));
	}
}


/*
 * Evaluates the output of a $hour expression.
 * Since a $hour is expressed as { "$hour": <dateExpression> }
 * or { "$hour": { date: <dateExpression>, timezone: <tzExpression> } }
 * We evaluate the inner expression and then return the hour part in the specified date expression with the specified timezone.
 */
void
HandleDollarHour(pgbson *doc, const bson_value_t *operatorValue,
				 ExpressionResult *expressionResult)
{
	HandleDatePartOperator(doc, operatorValue, expressionResult, "$hour", DatePart_Hour);
}


/*
 * Evaluates the output of a $minute expression.
 * Since a $minute is expressed as { "$minute": <dateExpression> }
 * or { "$minute": { date: <dateExpression>, timezone: <tzExpression> } }
 * We evaluate the inner expression and then return the minute part in the specified date expression with the specified timezone.
 */
void
HandleDollarMinute(pgbson *doc, const bson_value_t *operatorValue,
				   ExpressionResult *expressionResult)
{
	HandleDatePartOperator(doc, operatorValue, expressionResult, "$minute",
						   DatePart_Minute);
}


/*
 * Evaluates the output of a $second expression.
 * Since a $second is expressed as { "$second": <dateExpression> }
 * or { "$second": { date: <dateExpression>, timezone: <tzExpression> } }
 * We evaluate the inner expression and then return the second part in the specified date expression with the specified timezone.
 */
void
HandleDollarSecond(pgbson *doc, const bson_value_t *operatorValue,
				   ExpressionResult *expressionResult)
{
	HandleDatePartOperator(doc, operatorValue, expressionResult, "$second",
						   DatePart_Second);
}


/*
 * Evaluates the output of a $millisecond expression.
 * Since a $millisecond is expressed as { "$millisecond": <dateExpression> }
 * or { "$millisecond": { date: <dateExpression>, timezone: <tzExpression> } }
 * We evaluate the inner expression and then return the millisecond part in the specified date expression with the specified timezone.
 */
void
HandleDollarMillisecond(pgbson *doc, const bson_value_t *operatorValue,
						ExpressionResult *expressionResult)
{
	HandleDatePartOperator(doc, operatorValue, expressionResult, "$millisecond",
						   DatePart_Millisecond);
}


/*
 * Evaluates the output of a $year expression.
 * Since a $year is expressed as { "$year": <dateExpression> }
 * or { "$year": { date: <dateExpression>, timezone: <tzExpression> } }
 * We evaluate the inner expression and then return the year part in the specified date expression with the specified timezone.
 */
void
HandleDollarYear(pgbson *doc, const bson_value_t *operatorValue,
				 ExpressionResult *expressionResult)
{
	HandleDatePartOperator(doc, operatorValue, expressionResult, "$year", DatePart_Year);
}


/*
 * Evaluates the output of a $month expression.
 * Since a $month is expressed as { "$month": <dateExpression> }
 * or { "$month": { date: <dateExpression>, timezone: <tzExpression> } }
 * We evaluate the inner expression and then return the month part in the specified date expression with the specified timezone.
 */
void
HandleDollarMonth(pgbson *doc, const bson_value_t *operatorValue,
				  ExpressionResult *expressionResult)
{
	HandleDatePartOperator(doc, operatorValue, expressionResult, "$month",
						   DatePart_Month);
}


/*
 * Evaluates the output of a $week expression.
 * Since a $week is expressed as { "$week": <dateExpression> }
 * or { "$week": { date: <dateExpression>, timezone: <tzExpression> } }
 * We evaluate the inner expression and then return the week number [0-53 (leap year)] in the specified date expression with the specified timezone.
 */
void
HandleDollarWeek(pgbson *doc, const bson_value_t *operatorValue,
				 ExpressionResult *expressionResult)
{
	HandleDatePartOperator(doc, operatorValue, expressionResult, "$week", DatePart_Week);
}


/*
 * Evaluates the output of a $dayOfYear expression.
 * Since a $dayOfYear is expressed as { "$dayOfYear": <dateExpression> }
 * or { "$dayOfYear": { date: <dateExpression>, timezone: <tzExpression> } }
 * We evaluate the inner expression and then return the day of the year [1-366 (leap year)] in the specified date expression with the specified timezone.
 */
void
HandleDollarDayOfYear(pgbson *doc, const bson_value_t *operatorValue,
					  ExpressionResult *expressionResult)
{
	HandleDatePartOperator(doc, operatorValue, expressionResult, "$dayOfYear",
						   DatePart_DayOfYear);
}


/*
 * Evaluates the output of a $dayOfMonth expression.
 * Since a $dayOfMonth is expressed as { "$dayOfMonth": <dateExpression> }
 * or { "$dayOfMonth": { date: <dateExpression>, timezone: <tzExpression> } }
 * We evaluate the inner expression and then return the day of the month [1-31] in the specified date expression with the specified timezone.
 */
void
HandleDollarDayOfMonth(pgbson *doc, const bson_value_t *operatorValue,
					   ExpressionResult *expressionResult)
{
	HandleDatePartOperator(doc, operatorValue, expressionResult, "$dayOfMonth",
						   DatePart_DayOfMonth);
}


/*
 * Evaluates the output of a $dayOfWeek expression.
 * Since a $dayOfWeek is expressed as { "$dayOfWeek": <dateExpression> }
 * or { "$dayOfWeek": { date: <dateExpression>, timezone: <tzExpression> } }
 * We evaluate the inner expression and then return the day of the week [1 (Sunday) - 7 (Saturday)] in the specified date expression with the specified timezone.
 */
void
HandleDollarDayOfWeek(pgbson *doc, const bson_value_t *operatorValue,
					  ExpressionResult *expressionResult)
{
	HandleDatePartOperator(doc, operatorValue, expressionResult, "$dayOfWeek",
						   DatePart_DayOfWeek);
}


/*
 * Evaluates the output of a $isoWeekYear expression.
 * Since a $isoWeekYear is expressed as { "$isoWeekYear": <dateExpression> }
 * or { "$isoWeekYear": { date: <dateExpression>, timezone: <tzExpression> } }
 * We evaluate the inner expression and then return the year based on the ISO 8601 week numbering in the specified date expression with the specified timezone.
 * In ISO 8601 the year starts with the Monday of week 1 and ends with the Sunday of the last week.
 * So in early January or late December the ISO year may be different from the Gregorian year. See HandleDollarIsoWeek summary for more details.
 */
void
HandleDollarIsoWeekYear(pgbson *doc, const bson_value_t *operatorValue,
						ExpressionResult *expressionResult)
{
	HandleDatePartOperator(doc, operatorValue, expressionResult, "$isoWeekYear",
						   DatePart_IsoWeekYear);
}


/*
 * Evaluates the output of a $isoWeek expression.
 * Since a $isoWeek is expressed as { "$isoWeek": <dateExpression> }
 * or { "$isoWeek": { date: <dateExpression>, timezone: <tzExpression> } }
 * We evaluate the inner expression and then return the week based on the ISO 8601 week numbering in the specified date expression with the specified timezone.
 * Iso week start on Mondays and the first week of a year contains January 4 of that year. In other words, the first Thursday
 * of a year is in week 1 of that year. So it is possible for early-January dates to be part of the 52nd or 53rd week
 * of the previous year and for late-December dates to be part of the first week of the next year. i.e: 2005-01-01
 * in ISO 8601 is part of the 53rd week of year 2004
 */
void
HandleDollarIsoWeek(pgbson *doc, const bson_value_t *operatorValue,
					ExpressionResult *expressionResult)
{
	HandleDatePartOperator(doc, operatorValue, expressionResult, "$isoWeek",
						   DatePart_IsoWeek);
}


/*
 * Evaluates the output of a $isoDayOfWeek expression.
 * Since a $isoDayOfWeek is expressed as { "$isoDayOfWeek": <dateExpression> }
 * or { "$isoDayOfWeek": { date: <dateExpression>, timezone: <tzExpression> } }
 * We evaluate the inner expression and then return the day of week based on the ISO 8601 numbering in the specified date expression with the specified timezone.
 * ISO 8601 week start on Mondays (1) and end on Sundays (7).
 */
void
HandleDollarIsoDayOfWeek(pgbson *doc, const bson_value_t *operatorValue,
						 ExpressionResult *expressionResult)
{
	HandleDatePartOperator(doc, operatorValue, expressionResult, "$isoDayOfWeek",
						   DatePart_IsoDayOfWeek);
}


/*
 * Evaluates the output of a $dateToParts expression.
 * Since a $datToParts is expressed as { "$dateToParts": { date: <dateExpression>, [ timezone: <tzExpression>, iso8601: <boolExpression> ] } }
 * We validate the input document and return the date parts from the 'date' adjusted to the provided 'timezone'. The date parts that we return
 * depend on the iso8601 argument which defaults to false when not provided. This is the output based on is08601 value:
 *   True -> {"isoWeekYear": <val>, "isoWeek": <val>, "isoDayOfWeek": <val>, "hour": <val>, "minute": <val>, "second": <val>, "millisecond": <val> }
 *   False -> {"year": <val>, "month": <val>, "day": <val>, hour": <val>, "minute": <val>, "second": <val>, "millisecond": <val> }
 */
void
HandleDollarDateToParts(pgbson *doc, const bson_value_t *operatorValue,
						ExpressionResult *expressionResult)
{
	if (operatorValue->value_type != BSON_TYPE_DOCUMENT)
	{
		ereport(ERROR, (errcode(MongoLocation40524), errmsg(
							"$dateToParts only supports an object as its argument")));
	}

	bson_value_t dateExpression = { 0 };
	bson_value_t timezoneExpression = { 0 };
	bson_value_t isoArgExpression = { 0 };
	bson_iter_t documentIter = { 0 };
	BsonValueInitIterator(operatorValue, &documentIter);

	/* Parse the argument that should be in the following format:
	 * {"date": <dateExpression>, ["timezone": <stringExpression>, "iso8601": <boolExpression>]}. */
	while (bson_iter_next(&documentIter))
	{
		const char *key = bson_iter_key(&documentIter);
		if (strcmp(key, "date") == 0)
		{
			dateExpression = *bson_iter_value(&documentIter);
		}
		else if (strcmp(key, "timezone") == 0)
		{
			timezoneExpression = *bson_iter_value(&documentIter);
		}
		else if (strcmp(key, "iso8601") == 0)
		{
			isoArgExpression = *bson_iter_value(&documentIter);
		}
		else
		{
			ereport(ERROR, (errcode(MongoLocation40520), errmsg(
								"Unrecognized argument to $dateToParts: %s", key),
							errhint(
								"Unrecognized argument to $dateToParts, Unexpected key found in input")));
		}
	}

	if (dateExpression.value_type == BSON_TYPE_EOD)
	{
		ereport(ERROR, (errcode(MongoLocation40522), errmsg(
							"Missing 'date' parameter to $dateToParts")));
	}

	bool isNullOnEmpty = false;
	bson_value_t nullValue;
	nullValue.value_type = BSON_TYPE_NULL;

	/* If no timezone is specified, we don't apply any offset as that's the default timezone, UTC. */
	ExtensionTimezone timezoneToApply = {
		.offsetInMs = 0,
		.isUtcOffset = true,
	};

	/* A timezone was specified. */
	if (timezoneExpression.value_type != BSON_TYPE_EOD)
	{
		bson_value_t evaluatedTz = EvaluateExpressionAndGetValue(doc, &timezoneExpression,
																 expressionResult,
																 isNullOnEmpty);

		/* Match native mongo, if timezone resolves to null or undefined, we bail before
		 * evaluating the other arguments and return null. */
		if (IsExpressionResultNullOrUndefined(&evaluatedTz))
		{
			ExpressionResultSetValue(expressionResult, &nullValue);
			return;
		}

		if (evaluatedTz.value_type != BSON_TYPE_UTF8)
		{
			ThrowLocation40517Error(evaluatedTz.value_type);
		}

		StringView timezoneToParse = {
			.string = evaluatedTz.value.v_utf8.str,
			.length = evaluatedTz.value.v_utf8.len
		};

		timezoneToApply = ParseTimezone(timezoneToParse);
	}

	bool isIsoRequested = false;

	/* iso8601 argument was specified. */
	if (isoArgExpression.value_type != BSON_TYPE_EOD)
	{
		bson_value_t evaluatedIsoArg = EvaluateExpressionAndGetValue(doc,
																	 &isoArgExpression,
																	 expressionResult,
																	 isNullOnEmpty);

		/* Match native mongo, if iso8601 resolves to null or undefined, we bail before
		 * evaluating the other arguments and return null. */
		if (IsExpressionResultNullOrUndefined(&evaluatedIsoArg))
		{
			ExpressionResultSetValue(expressionResult, &nullValue);
			return;
		}

		if (evaluatedIsoArg.value_type != BSON_TYPE_BOOL)
		{
			ereport(ERROR, (errcode(MongoLocation40521), errmsg(
								"iso8601 must evaluate to a bool, found %s",
								BsonTypeName(evaluatedIsoArg.value_type)),
							errhint("iso8601 must evaluate to a bool, found %s",
									BsonTypeName(evaluatedIsoArg.value_type))));
		}

		isIsoRequested = evaluatedIsoArg.value.v_bool;
	}

	bson_value_t evaluatedDate = EvaluateExpressionAndGetValue(doc, &dateExpression,
															   expressionResult,
															   isNullOnEmpty);

	if (IsExpressionResultNullOrUndefined(&evaluatedDate))
	{
		ExpressionResultSetValue(expressionResult, &nullValue);
		return;
	}

	int64_t dateInMs = BsonValueAsDateTime(&evaluatedDate);
	Datum pgTimestamp = GetPgTimestampFromEpochWithTimezone(dateInMs, timezoneToApply);

	/* Get date parts and write them to the result. */
	pgbson_writer objectWriter;
	pgbson_element_writer *elementWriter =
		ExpressionResultGetElementWriter(expressionResult);

	PgbsonElementWriterStartDocument(elementWriter, &objectWriter);

	/* If iso8601 is true we should return an object in the following shape:
	 * {"isoWeekYear": <val>, "isoWeek": <val>, "isoDayOfWeek": <val>, "hour": <val>, "minute": <val>, "second": <val>, "millisecond": <val> }.
	 *
	 * If it is false we should return an oject with the following shape:
	 * {"year": <val>, "month": <val>, "day": <val>, hour": <val>, "minute": <val>, "second": <val>, "millisecond": <val> }. */

	if (isIsoRequested)
	{
		int isoWeekY = GetDatePartFromPgTimestamp(pgTimestamp, DatePart_IsoWeekYear);
		int isoWeek = GetDatePartFromPgTimestamp(pgTimestamp, DatePart_IsoWeek);
		int isoDoW = GetDatePartFromPgTimestamp(pgTimestamp, DatePart_IsoDayOfWeek);
		PgbsonWriterAppendInt32(&objectWriter, "isoWeekYear", 11, isoWeekY);
		PgbsonWriterAppendInt32(&objectWriter, "isoWeek", 7, isoWeek);
		PgbsonWriterAppendInt32(&objectWriter, "isoDayOfWeek", 12, isoDoW);
	}
	else
	{
		int year = GetDatePartFromPgTimestamp(pgTimestamp, DatePart_Year);
		int month = GetDatePartFromPgTimestamp(pgTimestamp, DatePart_Month);
		int dom = GetDatePartFromPgTimestamp(pgTimestamp, DatePart_DayOfMonth);
		PgbsonWriterAppendInt32(&objectWriter, "year", 4, year);
		PgbsonWriterAppendInt32(&objectWriter, "month", 5, month);
		PgbsonWriterAppendInt32(&objectWriter, "day", 3, dom);
	}

	int hour = GetDatePartFromPgTimestamp(pgTimestamp, DatePart_Hour);
	int minute = GetDatePartFromPgTimestamp(pgTimestamp, DatePart_Minute);
	int second = GetDatePartFromPgTimestamp(pgTimestamp, DatePart_Second);
	int ms = GetDatePartFromPgTimestamp(pgTimestamp, DatePart_Millisecond);
	PgbsonWriterAppendInt32(&objectWriter, "hour", 4, hour);
	PgbsonWriterAppendInt32(&objectWriter, "minute", 6, minute);
	PgbsonWriterAppendInt32(&objectWriter, "second", 6, second);
	PgbsonWriterAppendInt32(&objectWriter, "millisecond", 11, ms);

	PgbsonElementWriterEndDocument(elementWriter, &objectWriter);
	ExpressionResultSetValueFromWriter(expressionResult);
}


/*
 * Evaluates the output of a $dateToString expression.
 * Since a $datToString is expressed as { "$dateToString": { date: <dateExpression>, [ format: <strExpression>, timezone: <tzExpression>, onNull: <expression> ] } }
 * We validate the input document and return the string with the specified timezone using the specified format if any.
 * If no format is specified the default format is: %Y-%m-%dT%H:%M:%S.%LZ
 */
void
HandleDollarDateToString(pgbson *doc, const bson_value_t *operatorValue,
						 ExpressionResult *expressionResult)
{
	if (operatorValue->value_type != BSON_TYPE_DOCUMENT)
	{
		ereport(ERROR, (errcode(MongoLocation18629), errmsg(
							"$dateToString only supports an object as its argument")));
	}

	bson_value_t dateExpression = { 0 };
	bson_value_t formatExpression = { 0 };
	bson_value_t timezoneExpression = { 0 };
	bson_value_t onNullExpression = { 0 };
	bson_iter_t documentIter;
	BsonValueInitIterator(operatorValue, &documentIter);

	while (bson_iter_next(&documentIter))
	{
		const char *key = bson_iter_key(&documentIter);
		if (strcmp(key, "date") == 0)
		{
			dateExpression = *bson_iter_value(&documentIter);
		}
		else if (strcmp(key, "format") == 0)
		{
			formatExpression = *bson_iter_value(&documentIter);
		}
		else if (strcmp(key, "timezone") == 0)
		{
			timezoneExpression = *bson_iter_value(&documentIter);
		}
		else if (strcmp(key, "onNull") == 0)
		{
			onNullExpression = *bson_iter_value(&documentIter);
		}
		else
		{
			ereport(ERROR, (errcode(MongoLocation18534), errmsg(
								"Unrecognized argument to $dateToString: %s", key),
							errhint(
								"Unrecognized argument to $dateToString, Unexpected key found while parsing")));
		}
	}

	if (dateExpression.value_type == BSON_TYPE_EOD)
	{
		ereport(ERROR, (errcode(MongoLocation18628), errmsg(
							"Missing 'date' parameter to $dateToString")));
	}

	bool isNullOnEmpty = false;

	/* onNull is only used for when 'date' argument is null or undefined, so we default it to null for the other arguments. */
	bson_value_t onNullResult;
	onNullResult.value_type = BSON_TYPE_NULL;

	/* Initialize to keep compiler happy even though we only use it if a the format argument was specified, which initializes it. */
	StringView formatString = {
		.length = 0,
		.string = NULL,
	};

	/* A format string was specified as an argument. */
	if (formatExpression.value_type != BSON_TYPE_EOD)
	{
		bson_value_t evaluatedFormat = EvaluateExpressionAndGetValue(doc,
																	 &formatExpression,
																	 expressionResult,
																	 isNullOnEmpty);

		/* Match native mongo, if any argument is null when evaluating, bail and don't evaluate the rest of the args. */
		if (IsExpressionResultNullOrUndefined(&evaluatedFormat))
		{
			ExpressionResultSetValue(expressionResult, &onNullResult);
			return;
		}

		if (evaluatedFormat.value_type != BSON_TYPE_UTF8)
		{
			ereport(ERROR, (errcode(MongoLocation18533), errmsg(
								"$dateToString requires that 'format' be a string, found: %s with value %s",
								BsonTypeName(evaluatedFormat.value_type),
								BsonValueToJsonForLogging(&evaluatedFormat)),
							errhint(
								"$dateToString requires that 'format' be a string, found: %s",
								BsonTypeName(evaluatedFormat.value_type))));
		}

		formatString.length = evaluatedFormat.value.v_utf8.len;
		formatString.string = evaluatedFormat.value.v_utf8.str;
	}

	/* If no timezone is specified, we don't apply any offset as the default should be UTC. */
	ExtensionTimezone timezoneToApply = {
		.isUtcOffset = true,
		.offsetInMs = 0,
	};

	/* A timezone was specified. */
	if (timezoneExpression.value_type != BSON_TYPE_EOD)
	{
		bson_value_t evaluatedTimezone = EvaluateExpressionAndGetValue(doc,
																	   &timezoneExpression,
																	   expressionResult,
																	   isNullOnEmpty);

		/* Match native mongo, if any argument is null when evaluating, bail and don't evaluate the rest of the args. */
		if (IsExpressionResultNullOrUndefined(&evaluatedTimezone))
		{
			ExpressionResultSetValue(expressionResult, &onNullResult);
			return;
		}

		if (evaluatedTimezone.value_type != BSON_TYPE_UTF8)
		{
			ThrowLocation40517Error(evaluatedTimezone.value_type);
		}

		StringView timezoneToParse = {
			.string = evaluatedTimezone.value.v_utf8.str,
			.length = evaluatedTimezone.value.v_utf8.len,
		};

		timezoneToApply = ParseTimezone(timezoneToParse);
	}

	bson_value_t evaluatedDate = EvaluateExpressionAndGetValue(doc, &dateExpression,
															   expressionResult,
															   isNullOnEmpty);

	if (IsExpressionResultNullOrUndefined(&evaluatedDate))
	{
		/* onNull argument was specified. */
		if (onNullExpression.value_type != BSON_TYPE_EOD)
		{
			/* The onNull expression is only used if the 'date' argument is null. */
			bson_value_t evaluatedOnNull = EvaluateExpressionAndGetValue(doc,
																		 &onNullExpression,
																		 expressionResult,
																		 isNullOnEmpty);

			/* If the onNull expression evaluates to EOD, i.e: non existent path in the document,
			 * the result should be empty, so just bail. */
			if (evaluatedOnNull.value_type == BSON_TYPE_EOD)
			{
				return;
			}

			onNullResult = evaluatedOnNull;
		}

		ExpressionResultSetValue(expressionResult, &onNullResult);
		return;
	}

	int64_t dateTimeInMs = BsonValueAsDateTime(&evaluatedDate);

	StringView stringResult;
	if (formatExpression.value_type != BSON_TYPE_EOD)
	{
		stringResult =
			GetDateStringWithFormat(dateTimeInMs, timezoneToApply, formatString);
	}
	else
	{
		stringResult = GetDateStringWithDefaultFormat(dateTimeInMs, timezoneToApply,
													  DateStringFormatCase_UpperCase);
	}

	bson_value_t result = {
		.value_type = BSON_TYPE_UTF8,
		.value.v_utf8.str = (char *) stringResult.string,
		.value.v_utf8.len = stringResult.length,
	};

	ExpressionResultSetValue(expressionResult, &result);
}


/* --------------------------------------------------------- */
/* private helper methods. */
/* --------------------------------------------------------- */

/* Common method for operators that return a single date part. This method parses and validates the arguments and
 * sets the result into the specified ExpressionResult. For more details about every operator, see its
 * HandleDollar* method description. */
static void
HandleDatePartOperator(pgbson *doc, const bson_value_t *operatorValue,
					   ExpressionResult *expressionResult, const char *operatorName,
					   DatePart datePart)
{
	bson_value_t dateExpression;
	bson_value_t timezoneExpression;
	bool isTimezoneSpecified = ParseDatePartOperatorArgument(operatorValue, operatorName,
															 &dateExpression,
															 &timezoneExpression);
	bool isNullOnEmpty = false;
	bson_value_t evaluatedDate = EvaluateExpressionAndGetValue(doc, &dateExpression,
															   expressionResult,
															   isNullOnEmpty);

	if (IsExpressionResultNullOrUndefined(&evaluatedDate))
	{
		bson_value_t nullResult;
		nullResult.value_type = BSON_TYPE_NULL;
		ExpressionResultSetValue(expressionResult, &nullResult);
		return;
	}

	int64_t dateTimeInMs = BsonValueAsDateTime(&evaluatedDate);

	/* In case no timezone is specified we just apply a 0 offset to get the default timezone which is UTC.*/
	ExtensionTimezone timezoneToApply = {
		.offsetInMs = 0,
		.isUtcOffset = true,
	};

	/* If a timezone was specified, we need to evaluate the expression, validate, parse and apply it. */
	if (isTimezoneSpecified)
	{
		bson_value_t evaluatedTimezone = EvaluateExpressionAndGetValue(doc,
																	   &timezoneExpression,
																	   expressionResult,
																	   isNullOnEmpty);

		if (IsExpressionResultNullOrUndefined(&evaluatedTimezone))
		{
			bson_value_t nullResult;
			nullResult.value_type = BSON_TYPE_NULL;
			ExpressionResultSetValue(expressionResult, &nullResult);
			return;
		}

		if (evaluatedTimezone.value_type != BSON_TYPE_UTF8)
		{
			ereport(ERROR, (errcode(MongoLocation40533), errmsg(
								"%s requires a string for the timezone argument, but was given a %s (%s)",
								operatorName, BsonTypeName(evaluatedTimezone.value_type),
								BsonValueToJsonForLogging(&evaluatedTimezone)),
							errhint(
								"'%s' requires a string for the timezone argument, but was given a %s",
								operatorName, BsonTypeName(
									evaluatedTimezone.value_type))));
		}

		StringView timezone = {
			.string = evaluatedTimezone.value.v_utf8.str,
			.length = evaluatedTimezone.value.v_utf8.len,
		};

		timezoneToApply = ParseTimezone(timezone);
	}

	Datum pgTimestamp = GetPgTimestampFromEpochWithTimezone(dateTimeInMs,
															timezoneToApply);
	uint32_t datePartResult = GetDatePartFromPgTimestamp(pgTimestamp, datePart);

	bson_value_t result;
	if (datePart == DatePart_IsoWeekYear)
	{
		/* $isoWeekYear is a long in native mongo */
		result.value_type = BSON_TYPE_INT64;
		result.value.v_int64 = (int64_t) datePartResult;
	}
	else
	{
		result.value_type = BSON_TYPE_INT32;
		result.value.v_int32 = datePartResult;
	}

	ExpressionResultSetValue(expressionResult, &result);
}


/* This method parses the single date part operators argument.
 * which can be <expression>, [ <expression> ], { date: <expression>, timezone: <stringExpression> }
 * it does not validate that the date expression is a valid date expression, it just returns it in the specified pointer. */
static bool
ParseDatePartOperatorArgument(const bson_value_t *operatorValue, const char *operatorName,
							  bson_value_t *dateExpression,
							  bson_value_t *timezoneExpression)
{
	bool isTimezoneSpecified = false;

	if (operatorValue->value_type == BSON_TYPE_ARRAY)
	{
		bson_iter_t arrayIterator;
		BsonValueInitIterator(operatorValue, &arrayIterator);

		bool valid = bson_iter_next(&arrayIterator);
		if (valid)
		{
			*dateExpression = *bson_iter_value(&arrayIterator);

			/* array argument is only valid if it has a single element. */
			valid = !bson_iter_next(&arrayIterator);
		}

		if (!valid)
		{
			int numArgs = BsonDocumentValueCountKeys(operatorValue);
			ereport(ERROR, (errcode(MongoLocation40536), errmsg(
								"%s accepts exactly one argument if given an array, but was given %d",
								operatorName, numArgs),
							errhint(
								"%s accepts exactly one argument if given an array, but was given %d",
								operatorName, numArgs)));
		}
	}
	else if (operatorValue->value_type == BSON_TYPE_DOCUMENT)
	{
		/* a document argument could be an expression that evaluates to a date or
		 * an argument following the {date: <dateExpression>, timezone: <strExpression> } spec. */

		bool isDateSpecified = false;
		bson_iter_t documentIter;
		BsonValueInitIterator(operatorValue, &documentIter);

		if (bson_iter_next(&documentIter))
		{
			const char *key = bson_iter_key(&documentIter);
			if (bson_iter_key_len(&documentIter) > 1 && key[0] == '$')
			{
				/* Found an operator, need to evaluate. */
				*dateExpression = *operatorValue;
				return isTimezoneSpecified;
			}

			/* regular argument, try to get the date and timezone. */
			do {
				key = bson_iter_key(&documentIter);
				if (strcmp(key, "date") == 0)
				{
					isDateSpecified = true;
					*dateExpression = *bson_iter_value(&documentIter);
				}
				else if (strcmp(key, "timezone") == 0)
				{
					isTimezoneSpecified = true;
					*timezoneExpression = *bson_iter_value(&documentIter);
				}
				else
				{
					ereport(ERROR, (errcode(MongoLocation40535), errmsg(
										"unrecognized option to %s: \"%s\"",
										operatorName, key),
									errhint(
										"unrecognized option to operator %s, Unexpected key in input",
										operatorName)));
				}
			} while (bson_iter_next(&documentIter));
		}

		if (!isDateSpecified)
		{
			ereport(ERROR, (errcode(MongoLocation40539),
							errmsg(
								"missing 'date' argument to %s, provided: %s: %s",
								operatorName, operatorName, BsonValueToJsonForLogging(
									operatorValue)),
							errhint(
								"missing 'date' argument to %s, provided argument of type: %s",
								operatorName, BsonTypeName(operatorValue->value_type))));
		}
	}
	else
	{
		*dateExpression = *operatorValue;
	}

	return isTimezoneSpecified;
}


/* Helper method that constructs the date string for the dateInMs adjusted to the specified timezone.
 * The string will follow the format specified in the arguments. If the format is null or empty, the result is an empty string.
 * This method assumes that the specified timezone is already validated with ParseTimezone method. */
static StringView
GetDateStringWithFormat(int64_t dateInMs, ExtensionTimezone timezone, StringView format)
{
	StringView result = {
		.length = 0,
		.string = "",
	};

	Datum pgTimestamp = GetPgTimestampFromEpochWithTimezone(dateInMs, timezone);

	/* Special format requested. */
	if (format.length == 0 || format.string == NULL)
	{
		return result;
	}

	/* We get a buffer of size equal to the format length * 3 to get a large enough buffer to fit the final string in.
	 * With this we ensure it is large enough as the longest value produced for a date part is of length
	 * 5, and since every format identifier needs to have a %, the smallest format string with valid identifiers,
	 * is 2. i.e We get a format string such as %z which is ([+|-]hhmm), the result will be of length 5 and we
	 * will be allocating a string of length 6. */

	int bufferSize = format.length * 3;
	char *allocBuffer = NULL;
	char *buffer;
	char tmp[256];

	/* If we need less than 256 bytes, allocate it on the stack, if greater, on the heap. */
	if (bufferSize > 256)
	{
		allocBuffer = palloc0(sizeof(char) * bufferSize);
		buffer = allocBuffer;
	}
	else
	{
		buffer = tmp;
	}

	char *currentPtr = buffer;
	const char *end = buffer + bufferSize;
	char *formatPtr = (char *) format.string;

	uint32_t i = 0;
	uint32_t finalLength = 0;
	while (i < format.length)
	{
		if (*formatPtr != '%')
		{
			WriteCharAndAdvanceBuffer(&currentPtr, end, *formatPtr);
			formatPtr++, i++, finalLength++;
			continue;
		}

		if (i + 1 >= format.length)
		{
			ereport(ERROR, (errcode(MongoLocation18535), errmsg(
								"Unmatched '%%' at end of format string")));
		}

		/* Move to the format specifier. */
		i++;
		formatPtr++;

		DatePart datePart;
		int padding;
		switch (*formatPtr)
		{
			case 'd':
			{
				datePart = DatePart_DayOfMonth;
				padding = 2;
				break;
			}

			case 'G':
			{
				datePart = DatePart_IsoWeekYear;
				padding = 4;
				break;
			}

			case 'H':
			{
				datePart = DatePart_Hour;
				padding = 2;
				break;
			}

			case 'j':
			{
				datePart = DatePart_DayOfYear;
				padding = 3;
				break;
			}

			case 'L':
			{
				datePart = DatePart_Millisecond;
				padding = 3;
				break;
			}

			case 'm':
			{
				datePart = DatePart_Month;
				padding = 2;
				break;
			}

			case 'M':
			{
				datePart = DatePart_Minute;
				padding = 2;
				break;
			}

			case 'S':
			{
				datePart = DatePart_Second;
				padding = 2;
				break;
			}

			case 'w':
			{
				datePart = DatePart_DayOfWeek;
				padding = 1;
				break;
			}

			case 'u':
			{
				datePart = DatePart_IsoDayOfWeek;
				padding = 1;
				break;
			}

			case 'U':
			{
				datePart = DatePart_Week;
				padding = 2;
				break;
			}

			case 'V':
			{
				datePart = DatePart_IsoWeek;
				padding = 2;
				break;
			}

			case 'Y':
			{
				datePart = DatePart_Year;
				padding = 4;
				break;
			}

			case 'z':
			{
				/* Utc offset following the +/-[hh][mm] format */
				int offsetInMinutes = DetermineUtcOffsetForEpochWithTimezone(dateInMs,
																			 timezone);
				bool isNegative = offsetInMinutes < 0;
				offsetInMinutes = isNegative ? -offsetInMinutes : offsetInMinutes;

				WriteCharAndAdvanceBuffer(&currentPtr, end, isNegative ? '-' : '+');

				uint32_t hours = offsetInMinutes / MINUTES_IN_HOUR;
				uint32_t minutes = offsetInMinutes % MINUTES_IN_HOUR;
				WritePaddedUInt32AndAdvanceBuffer(&currentPtr, end, 2, hours);
				WritePaddedUInt32AndAdvanceBuffer(&currentPtr, end, 2, minutes);

				finalLength += 5;
				i++, formatPtr++;
				continue;
			}

			case 'Z':
			{
				/* Utc offset in minutes. */
				int offsetInMinutes = DetermineUtcOffsetForEpochWithTimezone(dateInMs,
																			 timezone);
				finalLength += WriteInt32AndAdvanceBuffer(&currentPtr, end,
														  offsetInMinutes);
				i++, formatPtr++;
				continue;
			}

			case '%':
			{
				WriteCharAndAdvanceBuffer(&currentPtr, end, '%');
				i++, formatPtr++, finalLength++;
				continue;
			}

			default:
			{
				ereport(ERROR, (errcode(MongoLocation18536), errmsg(
									"Invalid format character '%%%c' in format string",
									*formatPtr),
								errhint(
									"Invalid format character '%%%c' in format string",
									*formatPtr)));
			}
		}

		uint32_t datePartValue = GetDatePartFromPgTimestamp(pgTimestamp, datePart);
		ValidateDateValueIsInRange(datePartValue);
		WritePaddedUInt32AndAdvanceBuffer(&currentPtr, end, padding, datePartValue);

		finalLength += padding;
		formatPtr++;
		i++;
	}

	/* Can't use pnstrdup here as if the format string contains a null char, we are going to copy it over,
	 * and pnstrdup calculates the length if different from the argument provided to the first null byte
	 * found if any before the length provided, and we actually want to copy all chars as we built the string. */
	char *finalString = palloc0(sizeof(char) * (finalLength + 1));
	memcpy(finalString, buffer, sizeof(char) * finalLength);
	finalString[finalLength] = '\0';

	result.length = finalLength;
	result.string = finalString;

	if (allocBuffer != NULL)
	{
		/* We allocated on the heap and need to free the buffer. */
		pfree(allocBuffer);
	}

	return result;
}


/* Helper method that constructs the date string for the dateInMs adjusted to the specified timezone.
 * The string will follow the default format: %Y-%m-%dT%H:%M:%S.%LZ.
 * This method assumes that the specified timezone is already validated with ParseTimezone method.
 * @todo : We can optimise this function . Instead of making multiple calls we can think from the perspective to reduce calls to internal postgres function. Task: 2489730
 * */
StringView
GetDateStringWithDefaultFormat(int64_t dateInMs, ExtensionTimezone timezone,
							   DateStringFormatCase formatCase)
{
	Datum pgTimestamp = GetPgTimestampFromEpochWithTimezone(dateInMs, timezone);

	/* Default format is: %Y-%m-%dT%H:%M:%S.%LZ which is equivalent to: yyyy-mm-ddThh:mm:ss.msZ */
	char buffer[25] = { 0 };

	uint32_t year = GetDatePartFromPgTimestamp(pgTimestamp, DatePart_Year);
	ValidateDateValueIsInRange(year);

	uint32_t month = GetDatePartFromPgTimestamp(pgTimestamp, DatePart_Month);
	uint32_t day = GetDatePartFromPgTimestamp(pgTimestamp, DatePart_DayOfMonth);
	uint32_t hour = GetDatePartFromPgTimestamp(pgTimestamp, DatePart_Hour);
	uint32_t minute = GetDatePartFromPgTimestamp(pgTimestamp, DatePart_Minute);
	uint32_t second = GetDatePartFromPgTimestamp(pgTimestamp, DatePart_Second);
	uint32_t millisecond = GetDatePartFromPgTimestamp(pgTimestamp, DatePart_Millisecond);
	if (formatCase != DateStringFormatCase_LowerCase)
	{
		sprintf(buffer, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", year, month, day, hour,
				minute,
				second, millisecond);
	}
	else
	{
		sprintf(buffer, "%04d-%02d-%02dt%02d:%02d:%02d.%03dz", year, month, day, hour,
				minute,
				second, millisecond);
	}
	StringView result = {
		.length = 24,
		.string = strndup(buffer, 24),
	};

	return result;
}


/**  Helper method that constructs the timestamp string for the dateInMs adjusted to the specified timezone.
 * The string will follow the default format: Month Date Hours:Minutes:Seconds:Milliseconds.
 * This method assumes that the specified timezone is already validated with ParseTimezone method.
 * @param timeStampBsonElement : This is the specified timestamp bson type input
 * @param timezone : this is the adjusted specified timezone.
 * @todo : We can optimise this function . Instead of making multiple calls we can think from the perspective to reduce calls to internal postgres function. Task: 2489730
 * */
StringView
GetTimestampStringWithDefaultFormat(const bson_value_t *timeStampBsonElement,
									ExtensionTimezone timezone,
									DateStringFormatCase formatCase)
{
	Assert(BSON_TYPE_TIMESTAMP == timeStampBsonElement->value_type);

	int64_t dateInMs = timeStampBsonElement->value.v_timestamp.timestamp *
					   MILLISECONDS_IN_SECOND +
					   timeStampBsonElement->value.v_timestamp.increment;

	/* Default format is: Month name (3 letters) dd hours:minutes:seconds:milliseconds which is 19 chars*/
	char buffer[19] = { 0 };

	Datum pgTimestamp = GetPgTimestampFromEpochWithTimezone(dateInMs, timezone);

	uint32_t month = GetDatePartFromPgTimestamp(pgTimestamp, DatePart_Month);
	uint32_t day = GetDatePartFromPgTimestamp(pgTimestamp, DatePart_DayOfMonth);
	uint32_t hour = GetDatePartFromPgTimestamp(pgTimestamp, DatePart_Hour);
	uint32_t minute = GetDatePartFromPgTimestamp(pgTimestamp, DatePart_Minute);
	uint32_t second = GetDatePartFromPgTimestamp(pgTimestamp, DatePart_Second);
	uint32_t millisecond = GetDatePartFromPgTimestamp(pgTimestamp, DatePart_Millisecond);

	const char *formattedMonthName;
	switch (formatCase)
	{
		case DateStringFormatCase_UpperCase:
		{
			formattedMonthName = monthNamesUpperCase[month - 1];
			break;
		}

		case DateStringFormatCase_LowerCase:
		{
			formattedMonthName = monthNamesLowerCase[month - 1];
			break;
		}

		case DateStringFormatCase_CamelCase:
		default:
		{
			formattedMonthName = monthNamesCamelCase[month - 1];
			break;
		}
	}

	sprintf(buffer, "%s %02d %02d:%02d:%02d:%03d", formattedMonthName, day, hour, minute,
			second, millisecond);

	StringView result = {
		.length = 19,
		.string = strndup(buffer, 19),
	};

	return result;
}


/* Helper method that writes a signed int32 to the specified buffer and advances the buffer after writing.
 * It returns the length of the written string. */
static int
WriteInt32AndAdvanceBuffer(char **buffer, const char *end, int32_t value)
{
	char tmp[12]; /* We need at least 12 bytes for a signed int32 string. */
	int length = pg_ltoa(value, tmp);

	if ((*buffer + length) > end)
	{
		ereport(ERROR, (errmsg(
							"Buffer is not big enough to write the requested value for $dateToString.")));
	}

	memcpy(*buffer, tmp, sizeof(char) * length);
	*buffer += length;

	return length;
}


/* Helper method that writes an unsigned int32 padded with zeros using the specified padding.
 * This method also advances the buffer after writing the number of written characters which should be equivalent
 * to the specified padding. */
static void
WritePaddedUInt32AndAdvanceBuffer(char **buffer, const char *end, int padding,
								  uint32_t value)
{
	Assert(padding > 0 && padding <= 4);

	if ((*buffer + padding) > end)
	{
		ereport(ERROR, (errmsg(
							"Buffer is not big enough to write the requested value $dateToString.")));
	}

	char tmp[4];
	int actualLength = pg_ultoa_n(value, tmp);

	if (actualLength > padding)
	{
		ereport(ERROR, (errmsg("Value: %d has more digits than the requested padding: %d",
							   value, padding),
						errhint(
							"Value: %d has more digits than the requested padding: %d",
							value, padding)));
	}

	int numberOfZeros = padding - actualLength;

	if (numberOfZeros > 0)
	{
		memset(*buffer, '0', sizeof(char) * numberOfZeros);
		*buffer += numberOfZeros;
	}

	memcpy(*buffer, tmp, sizeof(char) * actualLength);
	*buffer += actualLength;
}


/* Helper method that writes a single char to the provided buffer and advances the buffer 1 place after writing. */
static void
WriteCharAndAdvanceBuffer(char **buffer, const char *end, char value)
{
	if ((*buffer + 1) > end)
	{
		ereport(ERROR, (errmsg(
							"Buffer is not big enough to write the requested value $dateToString.")));
	}

	**buffer = value;
	*buffer += 1;
}


/* Given a Unix epoch in milliseconds and a timezone (utc offset or an Olson Timezone Identifier) it returns a
 * Datum holding a TimestampTz instance adjusted to the provided timezone in order to use for date operations with postgres.
 * It assumes the timezone is valid and was created with the ParseTimezone method. */
static Datum
GetPgTimestampFromEpochWithTimezone(int64_t epochInMs, ExtensionTimezone timezone)
{
	if (timezone.isUtcOffset)
	{
		epochInMs += timezone.offsetInMs;
		return GetPgTimestampFromUnixEpoch(epochInMs);
	}


	return OidFunctionCall2(PostgresTimestampToZoneFunctionId(),
							CStringGetTextDatum(timezone.id),
							GetPgTimestampFromUnixEpoch(epochInMs));
}


/* Parses a timezone string that can be a utc offset or an Olson timezone identifier.
 * If it represents a utc offset it throws if it doesn't follow the mongo valid offset format: +/-[hh], +/-[hh][mm] or +/-[hh]:[mm].
 * Otherwise, it throws if the timezone doesn't exist. */
static ExtensionTimezone
ParseTimezone(StringView timezone)
{
	ExtensionTimezone result;
	if (timezone.length == 0 || timezone.string == NULL)
	{
		ThrowInvalidTimezoneIdentifier(timezone.string);
	}

	if (timezone.string[0] == '+' || timezone.string[0] == '-')
	{
		/* We've got a utc offset. */
		result.offsetInMs = ParseUtcOffset(timezone);
		result.isUtcOffset = true;
	}
	else
	{
		/* We've got an Timezone Identifier or an offset without a sign (which is not valid in native Mongo). */

		/* pg_tzset also accepts UTC offsets but format is different and more permisive than MongoDB,
		* so in case we got a result, we just check if it was an offset without a sign. i.e: 08:09 */
		const char *p = timezone.string;
		bool isOffset = true;
		while (*p)
		{
			if (!isdigit(*p) && *p != ':')
			{
				isOffset = false;
				break;
			}

			p++;
		}

		/* Check if timezone is supported -- pg_tzset loads the TZ from the TZ database
		 * and adds it to a cache if it exists. If it doesn't it returns NULL.
		 * It doesn't matter that we're loading the TZ to validate if it exists,
		 * as if it does, we are going to use it anyways and it will be in the cache already.
		 */
		if (isOffset || !pg_tzset(timezone.string))
		{
			ThrowInvalidTimezoneIdentifier(timezone.string);
		}

		result.id = timezone.string;
		result.isUtcOffset = false;
	}

	return result;
}


/* Helper method that returns the UTC offset for the provided timezone for the current epoch.
 * The UTC offset is returned in minutes.
 * This method assumes the timezone was already validated by using ParseTimezone method. */
static int32_t
DetermineUtcOffsetForEpochWithTimezone(int64_t epochInMs, ExtensionTimezone timezone)
{
	/* If the timezone is already a utc offset, return that value in minutes, otherwise
	 * use Postgres to determine the offset for the given date and timezone. */

	if (timezone.isUtcOffset)
	{
		return timezone.offsetInMs / MILLISECONDS_IN_SECOND / SECONDS_IN_MINUTE;
	}

	pg_tz *pgTz = pg_tzset(timezone.id);

	/* Should not be null, we should've already validated the timezone identifier. */
	Assert(pgTz != NULL);

	/* We need to represent the timestamp as a pg_tm object which holds the gmtoffset. */
	TimestampTz timestampTz = DatumGetTimestampTz(GetPgTimestampFromUnixEpoch(epochInMs));
	pg_time_t convertedTimeT = timestamptz_to_time_t(timestampTz);
	struct pg_tm *pgLocalTime = pg_localtime(&convertedTimeT, pgTz);

	return pgLocalTime->tm_gmtoff / SECONDS_IN_MINUTE;
}


/* Given a Utc offset string following mongo valid offsets, returns the offset difference in milliseconds.
 * Throws for non valid utc offsets. */
static int64_t
ParseUtcOffset(StringView offset)
{
	uint32_t hours = 0;
	uint32_t minutes = 0;
	bool valid = false;
	StringView start = StringViewSubstring(&offset, 1);
	switch (offset.length)
	{
		case 3:
		{
			/* "+/-[hh]" case */
			valid = TryParseTwoDigitNumber(start, &hours);
			break;
		}

		case 5:
		{
			/* "+/-[hh][mm]" case */
			valid = TryParseTwoDigitNumber(start, &hours);
			valid = valid && TryParseTwoDigitNumber(StringViewSubstring(&start, 2),
													&minutes);
			break;
		}

		case 6:
		{
			/* "+/-[hh]:[mm]" case */
			if (*(StringViewSubstring(&start, 2).string) == ':')
			{
				valid = TryParseTwoDigitNumber(start, &hours);
				valid = valid && TryParseTwoDigitNumber(StringViewSubstring(&start, 3),
														&minutes);
			}

			break;
		}

		default:
		{ }
	}


	if (!valid)
	{
		/* invalid input. */
		ThrowInvalidTimezoneIdentifier(offset.string);
	}

	uint64_t hrsInMs = hours * MINUTES_IN_HOUR * SECONDS_IN_MINUTE *
					   MILLISECONDS_IN_SECOND;
	uint64_t mntsInMs = minutes * SECONDS_IN_MINUTE * MILLISECONDS_IN_SECOND;
	uint64_t offsetInMs = hrsInMs + mntsInMs;

	return offset.string[0] == '-' ? -offsetInMs : offsetInMs;
}


/* Given a string representing a two digit number, converts it to an uint.
 * Returns true for valid two digit number representations, false otherwise. */
static bool
TryParseTwoDigitNumber(StringView str, uint32_t *result)
{
	char firstD = *str.string;
	char secondD = *(StringViewSubstring(&str, 1).string);

	if (isdigit(firstD) && isdigit(secondD))
	{
		*result = ((firstD - '0') * 10) + (secondD - '0');
		return true;
	}

	return false;
}


/* Given a Unix epoch in milliseconds returns a Datum containing a postgres Timestamp instance. */
static Datum
GetPgTimestampFromUnixEpoch(int64_t epochInMs)
{
	return OidFunctionCall1(PostgresToTimestamptzFunctionId(),
							Float8GetDatum(((float8) epochInMs) /
										   MILLISECONDS_IN_SECOND));
}


/* Returns the requested unit part from the provided postgres timestamp as an uint32. */
static uint32_t
GetDatePartFromPgTimestamp(Datum pgTimestamp, DatePart datePart)
{
	const char *partName;

	switch (datePart)
	{
		case DatePart_Hour:
		{
			partName = "hour";
			break;
		}

		case DatePart_Minute:
		{
			partName = "minute";
			break;
		}

		case DatePart_Second:
		{
			partName = "second";
			break;
		}

		case DatePart_Millisecond:
		{
			partName = "millisecond";
			break;
		}

		case DatePart_Year:
		{
			partName = "year";
			break;
		}

		case DatePart_Month:
		{
			partName = "month";
			break;
		}

		case DatePart_DayOfYear:
		{
			partName = "doy";
			break;
		}

		case DatePart_DayOfMonth:
		{
			partName = "day";
			break;
		}

		case DatePart_DayOfWeek:
		{
			partName = "dow";
			break;
		}

		case DatePart_IsoWeekYear:
		{
			partName = "isoyear";
			break;
		}

		case DatePart_IsoWeek:
		{
			partName = "week";
			break;
		}

		case DatePart_IsoDayOfWeek:
		{
			partName = "isodow";
			break;
		}

		case DatePart_Week:
		{
			/* In postgres the week part follows the ISO 8601 week numbering. Which start on Mondays and
			 * the first week of a year contains January 4 of that year. In other words, the first Thursday
			 * of a year is in week 1 of that year. So it is possible for early-January dates to be part of the 52nd or 53rd week
			 * of the previous year and for late-December dates to be part of the first week of the next year. i.e: 2005-01-01
			 * in ISO 8601 is part of the 53rd week of year 2004, in this case we should return 1 as we don't want the ISO week numbering.
			 * Native mongo for non-ISO weeks, weeks begin on Sundays and the first week begins the first Sunday of the year, days preceding
			 * the first Sunday of the year are in week 0. */

			uint32_t dayOfYear = GetDatePartFromPgTimestamp(pgTimestamp,
															DatePart_DayOfYear);
			uint32_t dayOfWeek = GetDatePartFromPgTimestamp(pgTimestamp,
															DatePart_DayOfWeek);

			/* Since weeks start on Sunday and the days of the year preceding the first Sunday of the year, we need to get the
			 * Day of year for the Sunday after the current day of year and then divide it by 7 to get the week on 0-53 range.
			 * previous Sunday should be the dayOfYear - dayOfWeek (since weeks start on Sunday, if negative, it means previous Sunday is part of week 0)
			 * then we can just add 7 (number of days in week) to get the next Sunday. */
			return ((dayOfYear - dayOfWeek) + DAYS_IN_WEEK) / DAYS_IN_WEEK;
		}

		default:
		{
			ereport(ERROR, (errcode(MongoBadValue),
							errmsg("Invalid date part unit %d", datePart),
							errhint("Invalid date part unit %d", datePart)));
		}
	}

	Datum partDatum = OidFunctionCall2(PostgresDatePartFunctionId(),
									   CStringGetTextDatum(partName), pgTimestamp);
	double float8 = DatumGetFloat8(partDatum);
	uint32_t result = (uint32_t) float8;

	if (datePart == DatePart_Millisecond)
	{
		/* In postgres the millisecond part includes full seconds, so we need to strip out the seconds and get the MS part only.
		 * We use round rather than downcast because precision can be lost when postgres gets the milliseconds from the date. */
		result = (uint32_t) round(float8);
		result = result % MILLISECONDS_IN_SECOND;
	}

	if (datePart == DatePart_DayOfWeek)
	{
		/* Postgres range for dow is 0-6 and native mongo uses 1-7 range */
		result = result + 1;
	}

	return result;
}


/*
 * New Method Implementation for aggrgegation operators
 */


/* This function handles $dateFromParts after parsing and gives the result as BSON_TYPE_DATETIME */
void
HandlePreParsedDollarDateFromParts(pgbson *doc, void *arguments,
								   ExpressionResult *expressionResult)
{
	DollarDateFromParts *dollarOpArgs = arguments;
	bool isNullOnEmpty = false;

	DollarDateFromPartsBsonValue dateFromPartsValue = {
		.hour = { 0 },
		.minute = { 0 },
		.second = { 0 },
		.millisecond = { 0 },
		.timezone = { 0 },
		.isoWeekYear = { 0 },
		.isoWeek = { 0 },
		.isoDayOfWeek = { 0 },
		.year = { 0 },
		.month = { 0 },
		.day = { 0 },
	};
	ExpressionResult childExpression = ExpressionResultCreateChild(expressionResult);
	EvaluateAggregationExpressionData(&dollarOpArgs->hour, doc,
									  &childExpression,
									  isNullOnEmpty);
	dateFromPartsValue.hour = childExpression.value;
	ExpressionResultReset(&childExpression);

	EvaluateAggregationExpressionData(&dollarOpArgs->minute, doc, &childExpression,
									  isNullOnEmpty);
	dateFromPartsValue.minute = childExpression.value;
	ExpressionResultReset(&childExpression);

	EvaluateAggregationExpressionData(&dollarOpArgs->second, doc, &childExpression,
									  isNullOnEmpty);
	dateFromPartsValue.second = childExpression.value;
	ExpressionResultReset(&childExpression);

	EvaluateAggregationExpressionData(&dollarOpArgs->millisecond, doc, &childExpression,
									  isNullOnEmpty);
	dateFromPartsValue.millisecond = childExpression.value;
	ExpressionResultReset(&childExpression);

	EvaluateAggregationExpressionData(&dollarOpArgs->timezone, doc, &childExpression,
									  isNullOnEmpty);
	dateFromPartsValue.timezone = childExpression.value;
	ExpressionResultReset(&childExpression);

	if (dollarOpArgs->isISOWeekDate)
	{
		/* If using ISO week date parts */
		EvaluateAggregationExpressionData(&dollarOpArgs->isoWeekYear, doc,
										  &childExpression,
										  isNullOnEmpty);
		dateFromPartsValue.isoWeekYear = childExpression.value;
		ExpressionResultReset(&childExpression);

		EvaluateAggregationExpressionData(&dollarOpArgs->isoWeek, doc, &childExpression,
										  isNullOnEmpty);
		dateFromPartsValue.isoWeek = childExpression.value;
		ExpressionResultReset(&childExpression);

		EvaluateAggregationExpressionData(&dollarOpArgs->isoDayOfWeek, doc,
										  &childExpression,
										  isNullOnEmpty);
		dateFromPartsValue.isoDayOfWeek = childExpression.value;
		ExpressionResultReset(&childExpression);
	}
	else
	{
		EvaluateAggregationExpressionData(&dollarOpArgs->year, doc, &childExpression,
										  isNullOnEmpty);
		dateFromPartsValue.year = childExpression.value;
		ExpressionResultReset(&childExpression);

		EvaluateAggregationExpressionData(&dollarOpArgs->month, doc, &childExpression,
										  isNullOnEmpty);
		dateFromPartsValue.month = childExpression.value;
		ExpressionResultReset(&childExpression);

		EvaluateAggregationExpressionData(&dollarOpArgs->day, doc, &childExpression,
										  isNullOnEmpty);
		dateFromPartsValue.day = childExpression.value;
		ExpressionResultReset(&childExpression);
	}


	bson_value_t result = { .value_type = BSON_TYPE_DATE_TIME };
	if (IsInputForDatePartNull(&dateFromPartsValue, dollarOpArgs->isISOWeekDate))
	{
		result.value_type = BSON_TYPE_NULL;
		ExpressionResultSetValue(expressionResult, &result);
		return;
	}

	ValidateInputForDateFromParts(&dateFromPartsValue, dollarOpArgs->isISOWeekDate);
	StringView timezoneToParse = {
		.string = dateFromPartsValue.timezone.value.v_utf8.str,
		.length = dateFromPartsValue.timezone.value.v_utf8.len
	};
	ExtensionTimezone timezoneToApply = ParseTimezone(timezoneToParse);
	if (!dollarOpArgs->isISOWeekDate)
	{
		SetResultForDateFromParts(&dateFromPartsValue, timezoneToApply, &result);
	}
	else
	{
		SetResultForDateFromIsoParts(&dateFromPartsValue, timezoneToApply, &result);
	}

	ExpressionResultSetValue(expressionResult, &result);
}


/* This function parses the input argument of the format : */

/* {
 *  $dateFromParts : {
 *      'year': <year>, 'month': <month>, 'day': <day>,
 *      'hour': <hour>, 'minute': <minute>, 'second': <second>,
 *      'millisecond': <ms>, 'timezone': <tzExpression>
 *  }
 * }
 * This function also validates the input expression and stores the result in case all the arguments are constant
 */
void
ParseDollarDateFromParts(const bson_value_t *argument, AggregationExpressionData *data)
{
	if (argument->value_type != BSON_TYPE_DOCUMENT)
	{
		ereport(ERROR, (errcode(MongoLocation40519), errmsg(
							"$dateFromParts only supports an object as its argument")));
	}

	bson_value_t year = { 0 };
	bson_value_t isoWeekYear = { 0 };
	bson_value_t month = { 0 };
	bson_value_t isoWeek = { 0 };
	bson_value_t isoDayOfWeek = { 0 };
	bson_value_t day = { 0 };
	bson_value_t hour = { 0 };
	bson_value_t minute = { 0 };
	bson_value_t second = { 0 };
	bson_value_t millisecond = { 0 };
	bson_value_t timezone = {
		.value_type = BSON_TYPE_UTF8, .value.v_utf8.str = "UTC", .value.v_utf8.len = 3
	};

	bool isIsoWeekDate;
	ParseInputForDateFromParts(argument, &year, &isoWeekYear, &month, &isoWeek,
							   &isoDayOfWeek, &day, &hour, &minute, &second, &millisecond,
							   &timezone, &isIsoWeekDate);

	DollarDateFromParts *dateFromParts = palloc0(sizeof(DollarDateFromParts));

	/* Set the value is iso date format or not. */
	dateFromParts->isISOWeekDate = isIsoWeekDate;

	if (dateFromParts->isISOWeekDate)
	{
		ParseAggregationExpressionData(&dateFromParts->isoWeekYear, &isoWeekYear);
		ParseAggregationExpressionData(&dateFromParts->isoWeek, &isoWeek);
		ParseAggregationExpressionData(&dateFromParts->isoDayOfWeek, &isoDayOfWeek);
	}
	else
	{
		ParseAggregationExpressionData(&dateFromParts->year, &year);
		ParseAggregationExpressionData(&dateFromParts->month, &month);
		ParseAggregationExpressionData(&dateFromParts->day, &day);
	}
	ParseAggregationExpressionData(&dateFromParts->hour, &hour);
	ParseAggregationExpressionData(&dateFromParts->minute, &minute);
	ParseAggregationExpressionData(&dateFromParts->second, &second);
	ParseAggregationExpressionData(&dateFromParts->millisecond, &millisecond);
	ParseAggregationExpressionData(&dateFromParts->timezone, &timezone);

	if (IsDollarDatePartsInputConstant(dateFromParts))
	{
		bson_value_t result = { .value_type = BSON_TYPE_DATE_TIME };

		DollarDateFromPartsBsonValue dateFromPartsBsonValue = {
			.hour = dateFromParts->hour.value,
			.minute = dateFromParts->minute.value,
			.second = dateFromParts->second.value,
			.millisecond = dateFromParts->millisecond.value,
			.timezone = dateFromParts->timezone.value,
			.isoWeekYear = dateFromParts->isoWeekYear.value,
			.isoWeek = dateFromParts->isoWeek.value,
			.isoDayOfWeek = dateFromParts->isoDayOfWeek.value,
			.year = dateFromParts->year.value,
			.month = dateFromParts->month.value,
			.day = dateFromParts->day.value,
		};
		if (IsInputForDatePartNull(&dateFromPartsBsonValue,
								   dateFromParts->isISOWeekDate))
		{
			result.value_type = BSON_TYPE_NULL;
			data->value = result;
			data->kind = AggregationExpressionKind_Constant;
			pfree(dateFromParts);
			return;
		}

		ValidateInputForDateFromParts(&dateFromPartsBsonValue,
									  dateFromParts->isISOWeekDate);
		StringView timezoneToParse = {
			.string = dateFromParts->timezone.value.value.v_utf8.str,
			.length = dateFromParts->timezone.value.value.v_utf8.len
		};
		ExtensionTimezone timezoneToApply = ParseTimezone(timezoneToParse);
		if (!dateFromParts->isISOWeekDate)
		{
			SetResultForDateFromParts(&dateFromPartsBsonValue, timezoneToApply, &result);
		}
		else
		{
			SetResultForDateFromIsoParts(&dateFromPartsBsonValue, timezoneToApply,
										 &result);
		}

		data->value = result;
		data->kind = AggregationExpressionKind_Constant;
		pfree(dateFromParts);
	}
	else
	{
		data->operator.arguments = dateFromParts;
		data->operator.argumentsKind = AggregationExpressionArgumentsKind_Palloc;
	}
}


/* A function to create postgres interval object given the amount of year, month, week, day, hour, second. */
static Datum
GetIntervalFromDatePart(int64 year, int64 month, int64 week, int64 day, int64 hour, int64
						minute, float8 second)
{
	return OidFunctionCall7(PostgresMakeIntervalFunctionId(),
							year, month, week, day, hour, minute, Float8GetDatum(second));
}


/* A function which when given a year tells if that year has 52 / 53 ISO weeks. */
/* This is required for adjusting the isoweeks when input of isoweeks is 0. */
/* We tend to reduce isoYear argument by 1 and set the value to isoWeek as 52/53. */
static int
GetIsoWeeksForYear(int64 year)
{
	int dowDec28 = GetDayOfWeek(year, 12, 28);

	/* ISO 8601 specifies that if December 28 is a Monday, then the year has 53 weeks. */
	/* If the day of the week for December 28 is Monday (1), then the year has 53 weeks. */
	/* Otherwise, it has 52 weeks. */
	return (dowDec28 == 1) ? 53 : 52;
}


/* Function to calculate the day of the week for the given year month and day
 *  Sakamoto’s algorithm is used here for its simplicity and effectiveness
 *  This proposes to give data in O(1) taking into account the leap year as well.
 */
static inline int
GetDayOfWeek(int year, int month, int day)
{
	static int t[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
	year -= month < 3;
	return (year + (year / 4) - (year / 100) + (year / 400) + t[month - 1] + day) % 7;
}


/* This is a helper function which just transforms the timestamp to a particular timezone. */
/* This function returns a datum containing instance of timestamp and not timestamptz. */
static Datum
GetPgTimestampAdjustedToTimezone(Datum timestamp, ExtensionTimezone
								 timezoneToApply)
{
	/* Timezone is utc offset need to make calculations in epoch */
	if (timezoneToApply.isUtcOffset)
	{
		/* changing the sign as original input is in UTC and +04:30 means we need to subtract -04:30 epochms to get utc timestamp */
		float8 offsetMsFloat = (float8) timezoneToApply.offsetInMs;
		float8 secondsAdjust = 0 - (offsetMsFloat / MILLISECONDS_IN_SECOND);
		Datum interval = GetIntervalFromDatePart(0, 0, 0, 0, 0, 0, secondsAdjust);
		return OidFunctionCall2(PostgresAddIntervalToTimestampFunctionId(),
								timestamp, interval);
	}

	/* Adjusting to timezone specified for olson. */
	return OidFunctionCall2(PostgresTimestampToZoneWithoutTzFunctionId(),
							CStringGetTextDatum(timezoneToApply.id), timestamp);
}


/* A utility function to check if the struct DollarDateFromParts has constant values. */
/* This helps us optimize the flow */
static inline bool
IsDollarDatePartsInputConstant(DollarDateFromParts *datePart)
{
	if (!IsAggregationExpressionConstant(&datePart->hour) ||
		!IsAggregationExpressionConstant(&datePart->minute) ||
		!IsAggregationExpressionConstant(&datePart->second) ||
		!IsAggregationExpressionConstant(&datePart->millisecond) ||
		!IsAggregationExpressionConstant(&datePart->timezone))
	{
		return false;
	}

	/* If datePart uses ISO week dates, check ISO week components */
	if (datePart->isISOWeekDate)
	{
		return IsAggregationExpressionConstant(&datePart->isoWeekYear) &&
			   IsAggregationExpressionConstant(&datePart->isoWeek) &&
			   IsAggregationExpressionConstant(&datePart->isoDayOfWeek);
	}

	/* Otherwise, check year, month, day, and minute components */
	return IsAggregationExpressionConstant(&datePart->year) &&
		   IsAggregationExpressionConstant(&datePart->month) &&
		   IsAggregationExpressionConstant(&datePart->day);
}


/* A function to check if any input part is null. */
/* In case of any null input we should not process any further and return null as output. */
static inline bool
IsInputForDatePartNull(DollarDateFromPartsBsonValue *dateFromPartsValue, bool
					   isIsoWeekDate)
{
	if (IsExpressionResultNullOrUndefined(&dateFromPartsValue->hour) ||
		IsExpressionResultNullOrUndefined(&dateFromPartsValue->minute) ||
		IsExpressionResultNullOrUndefined(&dateFromPartsValue->second) ||
		IsExpressionResultNullOrUndefined(&dateFromPartsValue->millisecond) ||
		IsExpressionResultNullOrUndefined(&dateFromPartsValue->timezone))
	{
		return true;
	}

	/* Check based on whether the date uses ISO week dates */
	if (isIsoWeekDate)
	{
		return IsExpressionResultNullOrUndefined(&dateFromPartsValue->isoWeekYear) ||
			   IsExpressionResultNullOrUndefined(&dateFromPartsValue->isoWeek) ||
			   IsExpressionResultNullOrUndefined(&dateFromPartsValue->isoDayOfWeek);
	}

	/* Check for null or undefined in standard date parts */
	return IsExpressionResultNullOrUndefined(&dateFromPartsValue->year) ||
		   IsExpressionResultNullOrUndefined(&dateFromPartsValue->month) ||
		   IsExpressionResultNullOrUndefined(&dateFromPartsValue->day);
}


/* A function which takes in the input argument of format $dateFromParts: {input document arg} */
/* It extracts the required fields from the document. */
static void
ParseInputForDateFromParts(const bson_value_t *argument, bson_value_t *year,
						   bson_value_t *isoWeekYear, bson_value_t *month,
						   bson_value_t *isoWeek,
						   bson_value_t *isoDayOfWeek, bson_value_t *day,
						   bson_value_t *hour, bson_value_t *minute,
						   bson_value_t *second, bson_value_t *millisecond,
						   bson_value_t *timezone, bool *isIsoWeekDate)
{
	*isIsoWeekDate = false;
	bson_iter_t docIter;
	BsonValueInitIterator(argument, &docIter);
	while (bson_iter_next(&docIter))
	{
		const char *key = bson_iter_key(&docIter);
		if (strcmp(key, "year") == 0)
		{
			*year = *bson_iter_value(&docIter);
		}
		else if (strcmp(key, "isoWeekYear") == 0)
		{
			*isoWeekYear = *bson_iter_value(&docIter);
			*isIsoWeekDate = true;
		}
		else if (strcmp(key, "month") == 0)
		{
			*month = *bson_iter_value(&docIter);
		}
		else if (strcmp(key, "isoWeek") == 0)
		{
			*isoWeek = *bson_iter_value(&docIter);
			*isIsoWeekDate = true;
		}
		else if (strcmp(key, "day") == 0)
		{
			*day = *bson_iter_value(&docIter);
		}
		else if (strcmp(key, "isoDayOfWeek") == 0)
		{
			*isoDayOfWeek = *bson_iter_value(&docIter);
			*isIsoWeekDate = true;
		}
		else if (strcmp(key, "hour") == 0)
		{
			*hour = *bson_iter_value(&docIter);
		}
		else if (strcmp(key, "minute") == 0)
		{
			*minute = *bson_iter_value(&docIter);
		}
		else if (strcmp(key, "second") == 0)
		{
			*second = *bson_iter_value(&docIter);
		}
		else if (strcmp(key, "millisecond") == 0)
		{
			*millisecond = *bson_iter_value(&docIter);
		}
		else if (strcmp(key, "timezone") == 0)
		{
			*timezone = *bson_iter_value(&docIter);
		}
		else
		{
			ereport(ERROR, (errcode(MongoLocation40518), errmsg(
								"Unrecognized argument to $dateFromParts: %s", key),
							errhint(
								"Unrecognized argument to $dateFromParts, unexpected key")));
		}
	}

	/* Both the isodate part and normal date part present */
	if (*isIsoWeekDate && year->value_type != BSON_TYPE_EOD)
	{
		ereport(ERROR, (errcode(MongoLocation40489), errmsg(
							"$dateFromParts does not allow mixing natural dates with ISO dates")));
	}

	/* In case of ISO date normal date part should not be present */
	if (*isIsoWeekDate && (month->value_type != BSON_TYPE_EOD || day->value_type !=
						   BSON_TYPE_EOD))
	{
		ereport(ERROR, (errcode(MongoLocation40525), errmsg(
							"$dateFromParts does not allow mixing natural dates with ISO dates")));
	}

	/* Either year or isoWeekYear is a must given it's type is iso format or non-iso format */
	if ((!(*isIsoWeekDate) && year->value_type == BSON_TYPE_EOD) ||
		(*isIsoWeekDate && isoWeekYear->value_type == BSON_TYPE_EOD))
	{
		ereport(ERROR, (errcode(MongoLocation40516), errmsg(
							"$dateFromParts requires either 'year' or 'isoWeekYear' to be present")));
	}

	/* Set default values if not present */
	if (!(*isIsoWeekDate) && month->value_type == BSON_TYPE_EOD)
	{
		SetDefaultValueForDatePart(month, BSON_TYPE_INT64, 1);
	}
	if (!(*isIsoWeekDate) && day->value_type == BSON_TYPE_EOD)
	{
		SetDefaultValueForDatePart(day, BSON_TYPE_INT64, 1);
	}

	if (*isIsoWeekDate && isoDayOfWeek->value_type == BSON_TYPE_EOD)
	{
		SetDefaultValueForDatePart(isoDayOfWeek, BSON_TYPE_INT64, 1);
	}
	if (*isIsoWeekDate && isoWeek->value_type == BSON_TYPE_EOD)
	{
		SetDefaultValueForDatePart(isoWeek, BSON_TYPE_INT64, 1);
	}

	if (hour->value_type == BSON_TYPE_EOD)
	{
		SetDefaultValueForDatePart(hour, BSON_TYPE_INT64, 0);
	}
	if (minute->value_type == BSON_TYPE_EOD)
	{
		SetDefaultValueForDatePart(minute, BSON_TYPE_INT64, 0);
	}
	if (second->value_type == BSON_TYPE_EOD)
	{
		SetDefaultValueForDatePart(second, BSON_TYPE_INT64, 0);
	}
	if (millisecond->value_type == BSON_TYPE_EOD)
	{
		SetDefaultValueForDatePart(millisecond, BSON_TYPE_INT64, 0);
	}
}


/* This function takes is all the date parts and verifies the input type as per the expected behaviour. */
static void
ValidateInputForDateFromParts(DollarDateFromPartsBsonValue *dateFromPartsValue, bool
							  isIsoWeekDate)
{
	/* Validating Input type */
	if (isIsoWeekDate)
	{
		ValidateDatePart(DatePart_IsoWeekYear, &dateFromPartsValue->isoWeekYear,
						 "isoWeekYear");
		ValidateDatePart(DatePart_IsoWeek, &dateFromPartsValue->isoWeek, "isoWeek");
		ValidateDatePart(DatePart_IsoDayOfWeek, &dateFromPartsValue->isoDayOfWeek,
						 "isoDayOfWeek");
	}
	else
	{
		ValidateDatePart(DatePart_Year, &dateFromPartsValue->year, "year");
		ValidateDatePart(DatePart_Month, &dateFromPartsValue->month, "month");
		ValidateDatePart(DatePart_DayOfMonth, &dateFromPartsValue->day, "day");
	}

	ValidateDatePart(DatePart_Hour, &dateFromPartsValue->hour, "hour");
	ValidateDatePart(DatePart_Minute, &dateFromPartsValue->minute, "minute");
	ValidateDatePart(DatePart_Second, &dateFromPartsValue->second, "second");
	ValidateDatePart(DatePart_Millisecond, &dateFromPartsValue->millisecond,
					 "millisecond");

	if (dateFromPartsValue->timezone.value_type != BSON_TYPE_UTF8)
	{
		ereport(ERROR, (errcode(MongoLocation40517), errmsg(
							"timezone must evaluate to a string, found %s", BsonTypeName(
								dateFromPartsValue->timezone.value_type)),
						errhint(
							"timezone must evaluate to a string, found %s", BsonTypeName(
								dateFromPartsValue->timezone.value_type)
							)));
	}
}


/* A function which based on the input date part type validates the input range and input type */
static void
ValidateDatePart(DatePart datePart, bson_value_t *inputValue, char *inputKey)
{
	/* Validate input type */
	bool checkFixedInteger = false;
	if (!BsonValueIsNumber(inputValue) || !IsBsonValueFixedInteger(inputValue) ||
		(datePart == DatePart_Millisecond && !IsBsonValue64BitInteger(inputValue,
																	  checkFixedInteger)))
	{
		ereport(ERROR, (errcode(MongoLocation40515), errmsg(
							"'%s' must evaluate to an integer, found %s with value :%s",
							inputKey, BsonTypeName(inputValue->value_type),
							BsonValueToJsonForLogging(inputValue)),
						errhint(
							"'%s' must evaluate to an integer, found %s", inputKey,
							BsonTypeName(inputValue->value_type))));
	}
	int64 datePartValue = BsonValueAsInt64(inputValue);

	/* Validate Range */
	switch (datePart)
	{
		case DatePart_Year:
		case DatePart_IsoWeekYear:
		{
			if (datePartValue < 1 || datePartValue > 9999)
			{
				ereport(ERROR, (errcode(datePart == DatePart_Year ? MongoLocation40523 :
										MongoLocation31095), errmsg(
									"'%s' must evaluate to an integer in the range 1 to 9999, found %s",
									inputKey, BsonValueToJsonForLogging(inputValue)),
								errhint(
									"'%s' must evaluate to an integer in the range 1 to 9999, found %ld",
									inputKey, datePartValue)));
			}

			break;
		}

		case DatePart_Month:
		case DatePart_DayOfMonth:
		case DatePart_IsoWeek:
		case DatePart_IsoDayOfWeek:
		case DatePart_Hour:
		case DatePart_Minute:
		{
			if (datePartValue < -32768 || datePartValue > 32767)
			{
				ereport(ERROR, (errcode(MongoLocation31034), errmsg(
									"'%s' must evaluate to a value in the range [-32768, 32767]; value %s is not in range",
									inputKey, BsonValueToJsonForLogging(inputValue)),
								errhint(
									"'%s' must evaluate to a value in the range [-32768, 32767]; value %ld is not in range",
									inputKey, datePartValue)));
			}
			break;
		}

		case DatePart_Second:
		case DatePart_Millisecond:
		{
			/* as per the tests we should throw error for bit values >= 2^54 */
			if (datePartValue >= 18014398509481984 || datePartValue <= -18014398509481984)
			{
				ereport(ERROR, (errcode(MongoDurationOverflow), errmsg(
									"Overflow casting from a lower-precision duration to a higher-precision duration"
									)));
			}
			break;
		}

		default:
		{
			break;
		}
	}

	/* Convert all the values to same base post validation. */
	inputValue->value_type = BSON_TYPE_INT64;
	inputValue->value.v_int64 = datePartValue;
}


/* A common helper function to set default value for date part. */
static inline void
SetDefaultValueForDatePart(bson_value_t *datePart, bson_type_t bsonType, int64
						   defaultValue)
{
	datePart->value.v_int64 = defaultValue;
	datePart->value_type = bsonType;
}


/*
 * This function sets the result value after processing input for non-ISO date parts.
 */
static void
SetResultForDateFromParts(DollarDateFromPartsBsonValue *dateFromPartsValue,
						  ExtensionTimezone
						  timezoneToApply, bson_value_t *result)
{
	Datum timestampStart = GetPgTimestampFromUnixEpoch(DATE_FROM_PART_START_DATE_MS);
	float8 millis = (float8) dateFromPartsValue->millisecond.value.v_int64;
	float8 secondsAdjustedWithMillis = dateFromPartsValue->second.value.v_int64 +
									   (millis / MILLISECONDS_IN_SECOND);

	Datum interval = GetIntervalFromDatePart(dateFromPartsValue->year.value.v_int64 - 1,
											 dateFromPartsValue->month.value.v_int64 - 1,
											 0,
											 dateFromPartsValue->day.value.v_int64 - 1,
											 dateFromPartsValue->hour.value.v_int64,
											 dateFromPartsValue->minute.value.v_int64,
											 secondsAdjustedWithMillis);

	Datum timestampPlusInterval = OidFunctionCall2(
		PostgresAddIntervalToTimestampFunctionId(),
		timestampStart, interval);

	Datum resultTimeWithZone;

	/* Optimization as all calculations are in UTC by default so, no need to change timezone. */
	if (!timezoneToApply.isUtcOffset && strcmp(timezoneToApply.id, defaultTimezone) == 0)
	{
		resultTimeWithZone = timestampPlusInterval;
	}
	else
	{
		resultTimeWithZone = GetPgTimestampAdjustedToTimezone(timestampPlusInterval,
															  timezoneToApply);
	}

	Datum resultEpoch = OidFunctionCall2(PostgresDatePartFunctionId(),
										 CStringGetTextDatum("epoch"),
										 resultTimeWithZone);

	result->value.v_datetime = (int64) (DatumGetFloat8(resultEpoch) * 1000);
}


/*
 * This function sets the result value after processing input for isoDateParts
 */
static void
SetResultForDateFromIsoParts(DollarDateFromPartsBsonValue *dateFromPartsValue,
							 ExtensionTimezone timezoneToApply,
							 bson_value_t *result)
{
	int64 isoYearValue = dateFromPartsValue->isoWeekYear.value.v_int64;
	int64 isoWeekDaysValue = dateFromPartsValue->isoDayOfWeek.value.v_int64;
	int64 isoWeekValue = dateFromPartsValue->isoWeek.value.v_int64;

	/* This is a special handling for 0 as in this case as  $dateFromPartsValue carries or subtracts the difference from other date parts to calculate the date. */
	/* In this case we have to subtract year by 1 and based on what year has isoWeek we need to set that value it could be 52 or 53. */
	if (isoWeekValue == 0)
	{
		isoYearValue -= 1;
		isoWeekValue = GetIsoWeeksForYear(isoYearValue);
	}

	char buffer[25] = { 0 };
	sprintf(buffer, "%ld-%ld-%ld", isoYearValue,
			isoWeekValue,
			isoWeekDaysValue);
	Datum toDateResult = OidFunctionCall2(PostgresToDateFunctionId(),
										  CStringGetTextDatum(buffer),
										  CStringGetTextDatum(isoDateFormat));
	int64 daysAdjust = 0;
	if (isoWeekDaysValue > 7)
	{
		daysAdjust += isoWeekDaysValue - 7;
	}
	else if (isoWeekDaysValue <= 0)
	{
		daysAdjust -= -isoWeekDaysValue + 7;
	}

	float8 secondsAdjustedWithMillis = dateFromPartsValue->second.value.v_int64 +
									   ((dateFromPartsValue->millisecond.value.v_int64 *
										 1.0) / MILLISECONDS_IN_SECOND);

	Datum interval = GetIntervalFromDatePart(0,
											 0,
											 0,
											 daysAdjust,
											 dateFromPartsValue->hour.value.v_int64,
											 dateFromPartsValue->minute.value.v_int64,
											 secondsAdjustedWithMillis);

	Datum datePlusInterval = OidFunctionCall2(PostgresAddIntervalToDateFunctionId(),
											  toDateResult, interval);

	Datum resultTimeWithZone;

	/* Optimization as all calculations are in UTC by default so, no need to change timezone. */
	if (!timezoneToApply.isUtcOffset && strcmp(timezoneToApply.id, defaultTimezone) == 0)
	{
		resultTimeWithZone = datePlusInterval;
	}
	else
	{
		resultTimeWithZone = GetPgTimestampAdjustedToTimezone(datePlusInterval,
															  timezoneToApply);
	}

	Datum resultEpoch = OidFunctionCall2(PostgresDatePartFunctionId(),
										 CStringGetTextDatum("epoch"),
										 resultTimeWithZone);

	result->value.v_datetime = (int64) (DatumGetFloat8(resultEpoch) * 1000);
}
