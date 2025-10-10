#!/usr/bin/env python3
"""
Test client for transbasket translation server.
Sends translation requests and validates responses.
"""

import requests
import json
import uuid
from datetime import datetime, timezone
import sys
import time


class TranslationClient:
    def __init__(self, base_url="http://localhost:8889"):
        self.base_url = base_url.rstrip('/')

    def health_check(self):
        """Check if server is healthy."""
        try:
            response = requests.get(f"{self.base_url}/health", timeout=5)
            print(f"Health Check: {response.status_code}")
            print(f"Response: {response.json()}\n")
            return response.status_code == 200
        except Exception as e:
            print(f"Health check failed: {e}\n")
            return False

    def translate(self, text, from_lang="kor", to_lang="eng",
                  test_uuid=None, test_timestamp=None):
        """
        Send translation request.

        Args:
            text: Text to translate
            from_lang: Source language (ISO 639-2, e.g., 'kor', 'eng', 'jpn')
            to_lang: Target language (ISO 639-2)
            test_uuid: Optional UUID for testing (generates new if None)
            test_timestamp: Optional timestamp for testing (uses current if None)

        Returns:
            Response object
        """
        # Generate UUID and timestamp if not provided
        request_uuid = test_uuid or str(uuid.uuid4())
        timestamp = test_timestamp or datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.%f")[:-3] + "Z"

        payload = {
            "timestamp": timestamp,
            "uuid": request_uuid,
            "from": from_lang,
            "to": to_lang,
            "text": text
        }

        print(f"Request:")
        print(json.dumps(payload, indent=2, ensure_ascii=False))
        print()

        try:
            response = requests.post(
                f"{self.base_url}/translate",
                json=payload,
                headers={"Content-Type": "application/json"},
                timeout=60
            )

            print(f"Status Code: {response.status_code}")
            print(f"Response:")

            try:
                response_json = response.json()
                print(json.dumps(response_json, indent=2, ensure_ascii=False))
            except:
                print(response.text)

            print("\n" + "="*60 + "\n")

            return response

        except requests.exceptions.Timeout:
            print("Error: Request timed out")
            return None
        except Exception as e:
            print(f"Error: {e}")
            return None


def test_valid_requests():
    """Test valid translation requests."""
    print("\n" + "="*60)
    print("Testing Valid Requests")
    print("="*60 + "\n")

    client = TranslationClient()

    # Test 1: Korean to English
    print("Test 1: Korean to English")
    print("-" * 60)
    client.translate("안녕하세요", "kor", "eng")

    # Test 2: English to Korean
    print("Test 2: English to Korean")
    print("-" * 60)
    client.translate("Hello, how are you?", "eng", "kor")

    # Test 3: Japanese to English
    print("Test 3: Japanese to English")
    print("-" * 60)
    client.translate("こんにちは", "jpn", "eng")

    # Test 4: Long text
    print("Test 4: Long text translation")
    print("-" * 60)
    long_text = """
    인공지능 기술의 발전은 우리 사회에 큰 변화를 가져오고 있습니다.
    특히 번역 기술의 발전으로 언어 장벽이 점차 낮아지고 있으며,
    전 세계 사람들이 더 쉽게 소통할 수 있게 되었습니다.
    """
    client.translate(long_text.strip(), "kor", "eng")


def test_invalid_requests():
    """Test invalid requests to verify error handling."""
    print("\n" + "="*60)
    print("Testing Invalid Requests")
    print("="*60 + "\n")

    client = TranslationClient()

    # Test 1: Invalid UUID
    print("Test 1: Invalid UUID (should return 422)")
    print("-" * 60)
    client.translate("Hello", "eng", "kor", test_uuid="invalid-uuid")

    # Test 2: Invalid language code
    print("Test 2: Invalid language code (should return 422)")
    print("-" * 60)
    client.translate("Hello", "xyz", "kor")

    # Test 3: Invalid timestamp format
    print("Test 3: Invalid timestamp (should return 422)")
    print("-" * 60)
    client.translate("Hello", "eng", "kor", test_timestamp="invalid-timestamp")

    # Test 4: Empty text
    print("Test 4: Empty text (should return 422)")
    print("-" * 60)
    client.translate("", "eng", "kor")

    # Test 5: Malformed JSON
    print("Test 5: Malformed JSON (should return 400)")
    print("-" * 60)
    try:
        response = requests.post(
            f"{client.base_url}/translate",
            data="{'invalid': json}",
            headers={"Content-Type": "application/json"},
            timeout=5
        )
        print(f"Status Code: {response.status_code}")
        print(f"Response: {response.text}\n")
    except Exception as e:
        print(f"Error: {e}\n")

    print("="*60 + "\n")


def test_uuid_preservation():
    """Test that UUID and timestamp are preserved in response."""
    print("\n" + "="*60)
    print("Testing UUID and Timestamp Preservation")
    print("="*60 + "\n")

    client = TranslationClient()

    test_uuid = "550e8400-e29b-41d4-a716-446655440000"
    test_timestamp = "2025-10-10T01:23:45.678Z"

    response = client.translate(
        "테스트",
        "kor",
        "eng",
        test_uuid=test_uuid,
        test_timestamp=test_timestamp
    )

    if response and response.status_code == 200:
        response_data = response.json()
        if response_data.get("uuid") == test_uuid:
            print("✓ UUID preserved correctly")
        else:
            print("✗ UUID NOT preserved")

        if response_data.get("timestamp") == test_timestamp:
            print("✓ Timestamp preserved correctly")
        else:
            print("✗ Timestamp NOT preserved")

    print("\n" + "="*60 + "\n")


def test_concurrent_requests():
    """Test concurrent translation requests."""
    print("\n" + "="*60)
    print("Testing Concurrent Requests")
    print("="*60 + "\n")

    import concurrent.futures

    client = TranslationClient()

    test_texts = [
        ("안녕하세요", "kor", "eng"),
        ("Hello", "eng", "kor"),
        ("ありがとう", "jpn", "eng"),
        ("Thank you", "eng", "jpn"),
        ("좋은 아침입니다", "kor", "eng"),
    ]

    print(f"Sending {len(test_texts)} concurrent requests...\n")

    start_time = time.time()

    with concurrent.futures.ThreadPoolExecutor(max_workers=5) as executor:
        futures = [
            executor.submit(client.translate, text, from_lang, to_lang)
            for text, from_lang, to_lang in test_texts
        ]

        results = [future.result() for future in concurrent.futures.as_completed(futures)]

    elapsed_time = time.time() - start_time

    success_count = sum(1 for r in results if r and r.status_code == 200)

    print(f"Completed {len(results)} requests in {elapsed_time:.2f} seconds")
    print(f"Success: {success_count}/{len(results)}")
    print("\n" + "="*60 + "\n")


def main():
    """Main test runner."""
    print("\n" + "="*60)
    print("Transbasket Translation Server Test Client")
    print("="*60)

    client = TranslationClient()

    # Health check first
    print("\n" + "="*60)
    print("Health Check")
    print("="*60 + "\n")

    if not client.health_check():
        print("Error: Server is not healthy. Please start the server first.")
        print("Run: ./bin/transbasket")
        sys.exit(1)

    # Run tests
    if len(sys.argv) > 1:
        test_type = sys.argv[1]

        if test_type == "valid":
            test_valid_requests()
        elif test_type == "invalid":
            test_invalid_requests()
        elif test_type == "uuid":
            test_uuid_preservation()
        elif test_type == "concurrent":
            test_concurrent_requests()
        elif test_type == "all":
            test_valid_requests()
            test_invalid_requests()
            test_uuid_preservation()
            test_concurrent_requests()
        else:
            print(f"Unknown test type: {test_type}")
            print("Usage: python test_client.py [valid|invalid|uuid|concurrent|all]")
            sys.exit(1)
    else:
        # Default: run all tests
        test_valid_requests()
        test_invalid_requests()
        test_uuid_preservation()
        test_concurrent_requests()

    print("All tests completed!")


if __name__ == "__main__":
    main()
